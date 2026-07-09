/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_verify.c - AOT plan verifier
 */

#include "xaot_verify.h"
#include "xaot_prepare.h"
#include "../base/xglobal_indices.h"
#include "../frontend/parser/xtype_ref.h"
#include "../ir/xi_effect.h"
#include "../ir/xi_escape.h"
#include "../runtime/value/xstruct_layout.h"
#include "../shared/xr_derive_flags.h"
#include "../stdlib/xstdlib_metadata.h"
#include <stdio.h>
#include <string.h>

static bool set_error(char *errbuf, size_t errbuf_len, const char *msg) {
    if (errbuf && errbuf_len > 0) {
        snprintf(errbuf, errbuf_len, "%s", msg ? msg : "AOT verifier error");
    }
    return false;
}

static const XgBodySummary *verify_find_evidence_body_by_func(const XgGlobalEvidence *ev,
                                                              XgFuncId func_id);
static XgFuncId verify_find_method_body_func_id(const XgGlobalEvidence *ev, XgMethodId method_id);
static bool verify_body_summary_anchor(const XaotBundle *bundle, const XiFunc *func,
                                       XgFuncId body_func_id, uint32_t body_effect_bits,
                                       uint32_t body_escape_bits, uint32_t body_evidence,
                                       const char *plan_name, char *errbuf, size_t errbuf_len);

static bool verify_stdlib_link_module_known(const char *name) {
    return xr_stdlib_metadata_link_dependency_module_known(name);
}

static bool verify_link_dependency_name_shape(const XgLinkDependencySummary *dep, char *errbuf,
                                              size_t errbuf_len) {
    const char *dot;
    char module[XG_LINK_DEP_NAME_MAX];
    size_t module_len;
    switch ((XgLinkDependencyKind) dep->kind) {
        case XG_LINK_DEP_EXTERN_DYLIB:
            return true;
        case XG_LINK_DEP_STDLIB_MODULE:
            if (strchr(dep->name, '.') || strchr(dep->name, '/') || strchr(dep->name, '\\'))
                return set_error(errbuf, errbuf_len,
                                 "AOT stdlib module link dependency has invalid name");
            if (!verify_stdlib_link_module_known(dep->name))
                return set_error(errbuf, errbuf_len,
                                 "AOT stdlib module link dependency is unknown");
            return true;
        case XG_LINK_DEP_STDLIB_SYMBOL:
            dot = strchr(dep->name, '.');
            if (!dot || dot == dep->name || dot[1] == '\0' || strchr(dot + 1, '.') ||
                strchr(dot + 1, '/') || strchr(dot + 1, '\\'))
                return set_error(errbuf, errbuf_len,
                                 "AOT stdlib symbol link dependency has invalid name");
            module_len = (size_t) (dot - dep->name);
            if (module_len >= sizeof(module))
                return set_error(errbuf, errbuf_len,
                                 "AOT stdlib symbol link dependency has invalid name");
            memcpy(module, dep->name, module_len);
            module[module_len] = '\0';
            if (!verify_stdlib_link_module_known(module))
                return set_error(errbuf, errbuf_len,
                                 "AOT stdlib symbol link dependency module is unknown");
            return true;
        default:
            return set_error(errbuf, errbuf_len, "AOT link dependency evidence has invalid kind");
    }
}

static bool verify_func_has_plan_recursive(const XaotBundle *bundle, const XiFunc *func,
                                           char *errbuf, size_t errbuf_len) {
    uint16_t ci;
    if (!func)
        return set_error(errbuf, errbuf_len, "NULL Xi function in AOT bundle");
    if (!xaot_bundle_find_func_plan(bundle, func))
        return set_error(errbuf, errbuf_len, "Xi function has no AOT function plan");
    for (ci = 0; ci < func->nchildren; ci++) {
        if (!verify_func_has_plan_recursive(bundle, func->children[ci], errbuf, errbuf_len))
            return false;
    }
    return true;
}

static bool verify_value_plan(const XaotValuePlan *plan, char *errbuf, size_t errbuf_len) {
    if (!plan || !plan->value)
        return set_error(errbuf, errbuf_len, "AOT value plan has no Xi value");
    if (!plan->rep.c_type)
        return set_error(errbuf, errbuf_len, "AOT value plan is missing C type");
    return true;
}

static bool verify_container_elem_plan(const XaotContainerElemPlan *elem, char *errbuf,
                                       size_t errbuf_len) {
    const XaotRepInfo *info;

    if (!elem || !elem->type)
        return set_error(errbuf, errbuf_len, "AOT container elem plan has no type");
    if (!elem->elem_name || !elem->c_type)
        return set_error(errbuf, errbuf_len, "AOT container elem plan is missing names");
    info = xaot_rep_info(elem->rep);
    if (!info)
        return set_error(errbuf, errbuf_len, "AOT container elem plan has invalid rep");
    if (elem->storage_rep != info->storage_rep)
        return set_error(errbuf, errbuf_len, "AOT container elem storage rep mismatch");
    return true;
}

static bool verify_container_plan(const XaotContainerTypePlan *type_plan, char *errbuf,
                                  size_t errbuf_len) {
    const XaotContainerPlan *plan;
    XaotContainerPlan scratch;

    if (!type_plan)
        return set_error(errbuf, errbuf_len, "AOT container plan is NULL");
    plan = &type_plan->plan;
    if (!plan->type)
        return set_error(errbuf, errbuf_len, "AOT container plan has no type");
    if (plan->type_key.fingerprint == 0)
        return set_error(errbuf, errbuf_len, "AOT container plan has no type key");
    if (!xaot_container_plan_for_type(plan->type, &scratch) ||
        !xaot_type_key_equal(&plan->type_key, &scratch.type_key))
        return set_error(errbuf, errbuf_len, "AOT container type key mismatches type");
    if ((plan->flags & XAOT_CONTAINER_TYPED_STORAGE) == 0)
        return set_error(errbuf, errbuf_len, "AOT container plan lacks typed storage flag");
    if (plan->kind == XAOT_CONTAINER_ARRAY) {
        if (plan->type->kind != XR_KIND_ARRAY && plan->type->kind != XR_KIND_VIEW &&
            plan->type->kind != XR_KIND_SPAN && plan->type->kind != XR_KIND_FIXED_ARRAY)
            return set_error(errbuf, errbuf_len, "AOT array container plan has wrong type");
        if ((plan->flags & XAOT_CONTAINER_RAW_DATA) == 0)
            return set_error(errbuf, errbuf_len, "AOT array container plan lacks raw data flag");
        return verify_container_elem_plan(&plan->elem, errbuf, errbuf_len);
    }
    if (plan->kind == XAOT_CONTAINER_SET) {
        if (plan->type->kind != XR_KIND_SET)
            return set_error(errbuf, errbuf_len, "AOT set container plan has wrong type");
        if ((plan->flags & XAOT_CONTAINER_DIRECT_HELPERS) == 0)
            return set_error(errbuf, errbuf_len, "AOT set container plan lacks direct helpers");
        return verify_container_elem_plan(&plan->elem, errbuf, errbuf_len);
    }
    if (plan->kind == XAOT_CONTAINER_MAP) {
        if (plan->type->kind != XR_KIND_MAP)
            return set_error(errbuf, errbuf_len, "AOT map container plan has wrong type");
        if (!verify_container_elem_plan(&plan->key, errbuf, errbuf_len) ||
            !verify_container_elem_plan(&plan->value, errbuf, errbuf_len))
            return false;
        if ((plan->flags & XAOT_CONTAINER_DIRECT_HELPERS) != 0 &&
            !((plan->key.storage_rep == XR_REP_I64 || plan->key.storage_rep == XR_REP_F64) &&
              (plan->value.storage_rep == XR_REP_I64 || plan->value.storage_rep == XR_REP_F64)))
            return set_error(errbuf, errbuf_len,
                             "AOT map direct helper plan has unsupported storage");
        return true;
    }
    return set_error(errbuf, errbuf_len, "AOT container plan has unknown kind");
}

static bool verify_enum_plan_payloads_fit_compact_aggregate(const XaotEnumPlan *plan) {
    const XiEnumData *ed = plan ? plan->enum_data : NULL;
    if (!plan || !ed || !ed->is_adt || plan->max_payload > XAOT_ENUM_SCALAR_PAYLOAD_CAP)
        return false;
    if (plan->member_count > 0 && !plan->members)
        return false;
    for (uint32_t i = 0; i < plan->member_count; i++) {
        const XiEnumMemberData *member = plan->members ? &plan->members[i] : NULL;
        if (member && member->payload_count > XAOT_ENUM_SCALAR_PAYLOAD_CAP)
            return false;
    }
    return true;
}

static uint8_t verify_enum_scalar_action_for(const XaotEnumPlan *plan) {
    return verify_enum_plan_payloads_fit_compact_aggregate(plan)
               ? XAOT_ENUM_SCALAR_COMPACT_AGGREGATE
               : XAOT_ENUM_SCALAR_RUNTIME_AGGREGATE;
}

static uint32_t verify_enum_scalar_evidence_for(const XaotEnumPlan *plan) {
    uint32_t evidence = 0;
    if (!plan)
        return 0;
    if (plan->layout_id != 0)
        evidence |= XAOT_ENUM_SCALAR_EV_LAYOUT_ID;
    if (verify_enum_plan_payloads_fit_compact_aggregate(plan)) {
        evidence |= XAOT_ENUM_SCALAR_EV_PAYLOAD_BOUND | XAOT_ENUM_SCALAR_EV_TYPED_UNION;
        if (plan->type_arg_count > 0)
            evidence |= XAOT_ENUM_SCALAR_EV_CONCRETE_TYPES;
    }
    return evidence;
}

static bool verify_enum_plan(const XaotBundle *bundle, const XaotEnumPlan *plan, char *errbuf,
                             size_t errbuf_len) {
    const XiEnumData *ed;
    uint16_t expected_max_payload;

    if (!bundle || !plan)
        return set_error(errbuf, errbuf_len, "AOT enum scalar plan is NULL");
    ed = plan->enum_data;
    if (!ed || !ed->is_adt)
        return set_error(errbuf, errbuf_len, "AOT enum scalar plan has no ADT enum");
    if (plan->module_index >= bundle->nmodules)
        return set_error(errbuf, errbuf_len, "AOT enum scalar plan module is out of range");
    if (!plan->c_type || plan->c_type[0] == '\0')
        return set_error(errbuf, errbuf_len, "AOT enum scalar plan is missing C type");
    if (plan->member_count != ed->member_count)
        return set_error(errbuf, errbuf_len, "AOT enum scalar plan member count is stale");
    if (plan->layout_id != ed->layout_id)
        return set_error(errbuf, errbuf_len, "AOT enum scalar plan layout id is stale");
    expected_max_payload = ed->max_payload > 0 ? (uint16_t) ed->max_payload : 0;
    if (plan->max_payload != expected_max_payload)
        return set_error(errbuf, errbuf_len, "AOT enum scalar plan payload bound is stale");
    if (plan->scalar_payload_cap != XAOT_ENUM_SCALAR_PAYLOAD_CAP)
        return set_error(errbuf, errbuf_len, "AOT enum scalar plan payload cap is stale");
    if (plan->scalar_action != verify_enum_scalar_action_for(plan))
        return set_error(errbuf, errbuf_len, "AOT enum scalar action does not re-derive");
    if (plan->scalar_evidence != verify_enum_scalar_evidence_for(plan))
        return set_error(errbuf, errbuf_len, "AOT enum scalar evidence is stale");
    return true;
}

static bool verify_array_storage_plan(const XaotBundle *bundle, const XaotArrayStoragePlan *plan,
                                      char *errbuf, size_t errbuf_len) {
    const XaotValuePlan *value_plan;
    const XaotContainerTypePlan *container_plan;

    if (!bundle || !plan)
        return set_error(errbuf, errbuf_len, "AOT array storage plan is NULL");
    if (!plan->func || !plan->value)
        return set_error(errbuf, errbuf_len, "AOT array storage plan lacks func/value");
    if (!plan->origin)
        return set_error(errbuf, errbuf_len, "AOT array storage plan has no origin");
    if ((plan->flags & (XAOT_ARRAY_STORAGE_READ | XAOT_ARRAY_STORAGE_MUTABLE)) == 0)
        return set_error(errbuf, errbuf_len, "AOT array storage plan has no access flags");
    if (!verify_container_elem_plan(&plan->elem, errbuf, errbuf_len))
        return false;
    value_plan = xaot_bundle_find_value_plan(bundle, plan->value);
    if (!value_plan)
        return set_error(errbuf, errbuf_len, "AOT array storage value has no value plan");
    container_plan = xaot_bundle_find_container_plan(bundle, plan->value->type);
    if ((!container_plan || container_plan->plan.kind != XAOT_CONTAINER_ARRAY ||
         (container_plan->plan.flags & XAOT_CONTAINER_RAW_DATA) == 0) &&
        (plan->value->op == XI_SLICE || plan->value->op == XI_PHI))
        container_plan = xaot_bundle_find_container_plan(bundle, plan->origin->type);
    if (!container_plan || container_plan->plan.kind != XAOT_CONTAINER_ARRAY ||
        (container_plan->plan.flags & XAOT_CONTAINER_RAW_DATA) == 0)
        return set_error(errbuf, errbuf_len, "AOT array storage value has no array plan");
    if (strcmp(container_plan->plan.elem.elem_name, plan->elem.elem_name) != 0 ||
        strcmp(container_plan->plan.elem.c_type, plan->elem.c_type) != 0 ||
        container_plan->plan.elem.rep != plan->elem.rep ||
        container_plan->plan.elem.storage_rep != plan->elem.storage_rep)
        return set_error(errbuf, errbuf_len, "AOT array storage elem mismatches container plan");
    return true;
}

static bool verify_array_cache_plan(const XaotBundle *bundle, const XaotArrayCachePlan *plan,
                                    char *errbuf, size_t errbuf_len) {
    const XaotArrayStoragePlan *storage_plan;

    if (!bundle || !plan)
        return set_error(errbuf, errbuf_len, "AOT array cache plan is NULL");
    if (!plan->func || !plan->value || !plan->storage_value)
        return set_error(errbuf, errbuf_len, "AOT array cache plan lacks func/value/storage");
    if ((plan->flags & XAOT_ARRAY_CACHE_DECLARE_LOCAL) == 0)
        return set_error(errbuf, errbuf_len, "AOT array cache plan lacks local declaration flag");
    if ((plan->flags & (XAOT_ARRAY_CACHE_READ | XAOT_ARRAY_CACHE_MUTABLE)) == 0)
        return set_error(errbuf, errbuf_len, "AOT array cache plan has no access flags");
    uint32_t source_flags =
        plan->flags &
        (XAOT_ARRAY_CACHE_VIEW | XAOT_ARRAY_CACHE_FRESH_RESULT | XAOT_ARRAY_CACHE_FILL_LOOP |
         XAOT_ARRAY_CACHE_NATIVE_LOCAL | XAOT_ARRAY_CACHE_CLASS_FIELD);
    if (source_flags == 0)
        return set_error(errbuf, errbuf_len, "AOT array cache plan lacks source flag");
    if ((source_flags & (source_flags - 1u)) != 0)
        return set_error(errbuf, errbuf_len, "AOT array cache plan has multiple source flags");
    if (!verify_container_elem_plan(&plan->elem, errbuf, errbuf_len))
        return false;

    storage_plan = xaot_bundle_find_array_storage_plan(bundle, plan->storage_value);
    if (!storage_plan)
        return set_error(errbuf, errbuf_len, "AOT array cache storage has no storage plan");
    if ((storage_plan->flags & XAOT_ARRAY_STORAGE_READ) == 0)
        return set_error(errbuf, errbuf_len, "AOT array cache storage is not readable");
    if ((plan->flags & XAOT_ARRAY_CACHE_MUTABLE) != 0 &&
        (storage_plan->flags & XAOT_ARRAY_STORAGE_MUTABLE) == 0)
        return set_error(errbuf, errbuf_len, "AOT array cache mutable flag exceeds storage plan");
    if (strcmp(storage_plan->elem.elem_name, plan->elem.elem_name) != 0 ||
        strcmp(storage_plan->elem.c_type, plan->elem.c_type) != 0 ||
        storage_plan->elem.rep != plan->elem.rep ||
        storage_plan->elem.storage_rep != plan->elem.storage_rep)
        return set_error(errbuf, errbuf_len, "AOT array cache elem mismatches storage plan");
    return true;
}

static bool verify_array_class_field_alloc_plan(const XaotBundle *bundle,
                                                const XaotArrayClassFieldAllocPlan *plan,
                                                char *errbuf, size_t errbuf_len) {
    const XaotArrayStoragePlan *storage_plan;
    const XaotArrayCachePlan *cache_plan;
    const XrAggregateLayout *layout;
    const XrAggregateFieldLayout *field;

    if (!bundle || !plan)
        return set_error(errbuf, errbuf_len, "AOT array class-field alloc plan is NULL");
    if (!plan->func || !plan->origin || !plan->store || !plan->class_data)
        return set_error(errbuf, errbuf_len,
                         "AOT array class-field alloc plan lacks func/origin/store/class");
    if (!xaot_bundle_find_value_plan(bundle, plan->origin))
        return set_error(errbuf, errbuf_len,
                         "AOT array class-field alloc origin has no value plan");
    if (!xaot_bundle_find_value_plan(bundle, plan->store))
        return set_error(errbuf, errbuf_len, "AOT array class-field alloc store has no value plan");
    if (!verify_container_elem_plan(&plan->elem, errbuf, errbuf_len))
        return false;

    layout = plan->class_data->instance_layout;
    if (!layout || plan->field_idx >= layout->field_count)
        return set_error(errbuf, errbuf_len,
                         "AOT array class-field alloc has invalid class field index");
    field = &layout->fields[plan->field_idx];
    if (!field || field->native_type != XR_NATIVE_ARRAY_REF)
        return set_error(errbuf, errbuf_len,
                         "AOT array class-field alloc target is not Array ref field");

    storage_plan = xaot_bundle_find_array_storage_plan(bundle, plan->origin);
    if (!storage_plan || (storage_plan->flags & XAOT_ARRAY_STORAGE_MUTABLE) == 0)
        return set_error(errbuf, errbuf_len,
                         "AOT array class-field alloc origin has no mutable storage plan");
    cache_plan = xaot_bundle_find_array_cache_plan(bundle, plan->origin);
    if (!cache_plan || (cache_plan->flags & XAOT_ARRAY_CACHE_MUTABLE) == 0 ||
        (cache_plan->flags & XAOT_ARRAY_CACHE_FILL_LOOP) == 0)
        return set_error(errbuf, errbuf_len,
                         "AOT array class-field alloc origin has no mutable fill-loop cache plan");
    if (strcmp(storage_plan->elem.elem_name, plan->elem.elem_name) != 0 ||
        strcmp(cache_plan->elem.elem_name, plan->elem.elem_name) != 0 ||
        storage_plan->elem.rep != plan->elem.rep || cache_plan->elem.rep != plan->elem.rep ||
        strcmp(storage_plan->elem.c_type, plan->elem.c_type) != 0 ||
        strcmp(cache_plan->elem.c_type, plan->elem.c_type) != 0)
        return set_error(errbuf, errbuf_len,
                         "AOT array class-field alloc elem mismatches storage/cache plan");
    return true;
}

static bool verify_func_attr_value_is_ignorable_err_check(const XiValue *value) {
    return value && value->op == XI_ERR_CHECK &&
           (!value->type || value->type->kind != XR_KIND_BOOL);
}

/* Re-derive the effect evidence behind a function attribute plan.
 * CONST must touch no memory at all; PURE must never write / throw /
 * suspend. A stale or wrong plan would make the C compiler CSE calls
 * with observable effects, so any mismatch is a hard fail. */
static bool verify_func_attr_plan(const XaotBundle *bundle, const XaotFuncAttrPlan *plan,
                                  char *errbuf, size_t errbuf_len) {
    const XgGlobalEvidence *ev = bundle ? bundle->global_evidence_plan.evidence : NULL;
    const XgBodySummary *body;
    uint32_t expected_evidence;
    uint32_t composed_effect_bits = 0;
    uint32_t bi, vi;
    uint32_t closed_world_call_ops = 0;
    uint32_t closed_world_callsite_count = 0;
    bool reads_mem;
    bool closed_world_calls_composed;

    if (!bundle || !plan)
        return set_error(errbuf, errbuf_len, "AOT function attribute plan is NULL");
    if (!plan->func)
        return set_error(errbuf, errbuf_len, "AOT function attribute plan lacks func");
    if (plan->flags != XAOT_FN_ATTR_CONST && plan->flags != XAOT_FN_ATTR_PURE)
        return set_error(errbuf, errbuf_len, "AOT function attribute plan has invalid flags");
    expected_evidence = XAOT_FN_ATTR_EV_BODY_SUMMARY | XAOT_FN_ATTR_EV_XI_EFFECT_SCAN;
    if ((plan->body_effect_bits & XG_BODY_MAY_CALL) != 0)
        expected_evidence |= XAOT_FN_ATTR_EV_CALLEE_SUMMARY;
    if (plan->evidence != expected_evidence)
        return set_error(errbuf, errbuf_len, "AOT function attribute plan lacks evidence");
    body = verify_find_evidence_body_by_func(ev, plan->body_func_id);
    if (!body)
        return set_error(errbuf, errbuf_len, "AOT function attribute plan has no body summary");
    if (plan->func->name && body->name_id != xg_name_id(plan->func->name))
        return set_error(errbuf, errbuf_len, "AOT function attribute plan body identity is stale");
    if (plan->body_effect_bits != body->effect_bits)
        return set_error(errbuf, errbuf_len, "AOT function attribute effect bits are stale");
    if (plan->body_escape_bits != body->escape_bits)
        return set_error(errbuf, errbuf_len, "AOT function attribute escape bits are stale");
    if (!xg_body_effects_compose_closed_world_calls(ev, body, &composed_effect_bits))
        return set_error(errbuf, errbuf_len,
                         "AOT function attribute plan has unresolved call effects");
    if ((composed_effect_bits & (XG_BODY_MAY_THROW | XG_BODY_MAY_SUSPEND | XG_BODY_MAY_ALLOC |
                                 XG_BODY_MAY_MUTATE | XG_BODY_MAY_CALL_NATIVE)) != 0)
        return set_error(errbuf, errbuf_len,
                         "AOT function attribute plan contradicts body summary");
    if (!xaot_bundle_find_func_plan(bundle, plan->func))
        return set_error(errbuf, errbuf_len, "AOT function attribute plan func has no func plan");

    closed_world_calls_composed = (body->effect_bits & XG_BODY_MAY_CALL) != 0;
    closed_world_callsite_count = closed_world_calls_composed ? body->callsite_count : 0;
    reads_mem = ((composed_effect_bits & XG_BODY_MAY_READ_MEM) != 0) || plan->func->ncaptures > 0;
    for (bi = 0; bi < plan->func->nblocks; bi++) {
        const XiBlock *blk = plan->func->blocks[bi];
        if (!blk)
            continue;
        for (vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            bool composed_call = false;
            if (!v)
                continue;
            switch ((XiOp) v->op) {
                case XI_CALL:
                case XI_CALL_METHOD:
                case XI_CALL_METHOD_DIRECT:
                case XI_TAIL_CALL:
                    if (!closed_world_calls_composed)
                        return set_error(errbuf, errbuf_len,
                                         "AOT function attribute plan func contains a call");
                    closed_world_call_ops++;
                    if (closed_world_call_ops > closed_world_callsite_count)
                        return set_error(
                            errbuf, errbuf_len,
                            "AOT function attribute plan has extra closed-world calls");
                    composed_call = true;
                    break;
                case XI_CALL_BUILTIN:
                case XI_CLOSURE_NEW:
                case XI_GO:
                case XI_THREAD_SPAWN:
                case XI_AWAIT:
                case XI_PRINT:
                    return set_error(errbuf, errbuf_len,
                                     "AOT function attribute plan func contains a call");
                case XI_LOAD_UPVAL:
                case XI_GET_SHARED:
                case XI_GET_GLOBAL:
                    reads_mem = true;
                    break;
                default:
                    break;
            }
            if (v->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND |
                            XI_FLAG_WRITES_MEM) &&
                !verify_func_attr_value_is_ignorable_err_check(v) && !composed_call)
                return set_error(errbuf, errbuf_len,
                                 "AOT function attribute plan func has effectful value");
            if (!composed_call) {
                if (xi_op_allocates(v->op))
                    return set_error(errbuf, errbuf_len,
                                     "AOT function attribute plan func allocates");
                if (v->flags & XI_FLAG_READS_MEM)
                    reads_mem = true;
            }
        }
    }
    if (reads_mem && plan->flags == XAOT_FN_ATTR_CONST)
        return set_error(errbuf, errbuf_len,
                         "AOT function attribute plan claims const but func reads memory");
    return true;
}

/* Re-derive the in-bounds proof behind a bounds plan. A stale or wrong plan
 * would make Cgen emit unchecked element access, so any mismatch between the
 * recorded evidence and a fresh proof is a hard fail. Unproven entries are
 * audit rows: they must carry a reason and still fail to prove. */
static bool verify_bounds_plan(const XaotBundle *bundle, const XaotBoundsPlan *plan, char *errbuf,
                               size_t errbuf_len) {
    uint8_t reason = XAOT_BOUNDS_UNPROVEN_NONE;

    if (!bundle || !plan)
        return set_error(errbuf, errbuf_len, "AOT bounds plan is NULL");
    if (!plan->func || !plan->access)
        return set_error(errbuf, errbuf_len, "AOT bounds plan lacks func or access");
    if (plan->access->op != XI_INDEX_GET && plan->access->op != XI_INDEX_SET)
        return set_error(errbuf, errbuf_len, "AOT bounds plan access is not an index op");
    if (plan->access->nargs < 2)
        return set_error(errbuf, errbuf_len, "AOT bounds plan access lacks index argument");
    if (!xaot_bundle_find_func_plan(bundle, plan->func))
        return set_error(errbuf, errbuf_len, "AOT bounds plan func has no func plan");
    if (!verify_body_summary_anchor(bundle, plan->func, plan->body_func_id, plan->body_effect_bits,
                                    plan->body_escape_bits, plan->body_evidence, "bounds", errbuf,
                                    errbuf_len))
        return false;
    if ((plan->evidence == 0) == (plan->unproven_reason == XAOT_BOUNDS_UNPROVEN_NONE))
        return set_error(errbuf, errbuf_len, "AOT bounds plan evidence/reason are inconsistent");
    if (plan->evidence !=
        xaot_prepare_array_access_bounds_evidence(bundle, plan->func, plan->access, &reason))
        return set_error(errbuf, errbuf_len, "AOT bounds plan evidence does not re-derive");
    if (plan->unproven_reason != reason)
        return set_error(errbuf, errbuf_len, "AOT bounds plan unproven reason does not re-derive");
    return true;
}

static bool verify_span_access_plan(const XaotBundle *bundle, const XaotSpanAccessPlan *plan,
                                    char *errbuf, size_t errbuf_len) {
    XaotSpanAccessPlan derived;

    if (!bundle || !plan)
        return set_error(errbuf, errbuf_len, "AOT Span access plan is NULL");
    if (!plan->func || !plan->value)
        return set_error(errbuf, errbuf_len, "AOT Span access plan lacks func or value");
    if (!xaot_bundle_find_func_plan(bundle, plan->func))
        return set_error(errbuf, errbuf_len, "AOT Span access plan func has no func plan");
    if (!verify_body_summary_anchor(bundle, plan->func, plan->body_func_id, plan->body_effect_bits,
                                    plan->body_escape_bits, plan->body_evidence, "Span access",
                                    errbuf, errbuf_len))
        return false;
    if ((plan->eliminated_checks == 0) == (plan->unproven_reason == XAOT_SPAN_UNPROVEN_NONE))
        return set_error(errbuf, errbuf_len, "AOT Span access plan drop/reason are inconsistent");
    if (!xaot_prepare_span_access_plan_for_value(bundle, plan->func, plan->value, &derived))
        return set_error(errbuf, errbuf_len, "AOT Span access plan value no longer re-derives");
    if (plan->kind != derived.kind)
        return set_error(errbuf, errbuf_len, "AOT Span access plan kind does not re-derive");
    if (plan->evidence != derived.evidence)
        return set_error(errbuf, errbuf_len, "AOT Span access plan evidence does not re-derive");
    if (plan->eliminated_checks != derived.eliminated_checks)
        return set_error(errbuf, errbuf_len, "AOT Span access plan drops do not re-derive");
    if (plan->unproven_reason != derived.unproven_reason)
        return set_error(errbuf, errbuf_len, "AOT Span access plan reason does not re-derive");
    return true;
}

/* Re-derive the uniqueness proof behind an alias plan. A wrong restrict is
 * undefined behaviour in the generated C, so any mismatch between recorded
 * and freshly derived evidence is a hard fail. */
static bool verify_alias_plan(const XaotBundle *bundle, const XaotAliasPlan *plan, char *errbuf,
                              size_t errbuf_len) {
    const XaotArrayCachePlan *cache_plan;

    if (!bundle || !plan)
        return set_error(errbuf, errbuf_len, "AOT alias plan is NULL");
    if (!plan->func || !plan->value)
        return set_error(errbuf, errbuf_len, "AOT alias plan lacks func or value");
    if (plan->kind != XAOT_ALIAS_UNIQUE_DATA)
        return set_error(errbuf, errbuf_len, "AOT alias plan has unsupported kind");
    if (plan->evidence == 0)
        return set_error(errbuf, errbuf_len, "AOT alias plan lacks evidence");
    if (!xaot_bundle_find_func_plan(bundle, plan->func))
        return set_error(errbuf, errbuf_len, "AOT alias plan func has no func plan");
    if (!verify_body_summary_anchor(bundle, plan->func, plan->body_func_id, plan->body_effect_bits,
                                    plan->body_escape_bits, plan->body_evidence, "alias", errbuf,
                                    errbuf_len))
        return false;
    cache_plan = xaot_bundle_find_array_cache_plan(bundle, plan->value);
    if (!cache_plan)
        return set_error(errbuf, errbuf_len, "AOT alias plan has no backing array cache plan");
    if (plan->evidence != xaot_prepare_array_cache_alias_evidence(bundle, plan->func, cache_plan))
        return set_error(errbuf, errbuf_len, "AOT alias plan evidence does not re-derive");
    return true;
}

static uint32_t verify_allocation_evidence_for(const XiValue *value, uint16_t original_op) {
    uint32_t evidence = 0;
    if (!value)
        return 0;
    if (value->op == XI_STACK_ALLOC)
        evidence |= XAOT_ALLOC_EV_STACK_ALLOC_OP;
    if (value->escape == (uint8_t) XI_ESC_NONE)
        evidence |= XAOT_ALLOC_EV_NO_ESCAPE;
    if (original_op != 0 && xi_op_is_heap_alloc(original_op))
        evidence |= XAOT_ALLOC_EV_ORIGINAL_ALLOC_OP;
    evidence |= XAOT_ALLOC_EV_BODY_SUMMARY;
    return evidence;
}

static bool verify_allocation_plan_candidate(const XiValue *value) {
    if (!value || value->op != XI_STACK_ALLOC || value->aux_int <= 0)
        return false;
    return xi_op_is_heap_alloc((uint16_t) value->aux_int);
}

static bool verify_allocation_plan(const XaotBundle *bundle, const XaotAllocationPlan *plan,
                                   char *errbuf, size_t errbuf_len) {
    uint16_t original_op;
    uint32_t expected_evidence;

    if (!bundle || !plan)
        return set_error(errbuf, errbuf_len, "AOT allocation plan is NULL");
    if (!plan->func || !plan->value)
        return set_error(errbuf, errbuf_len, "AOT allocation plan lacks func or value");
    if (!xaot_bundle_find_func_plan(bundle, plan->func))
        return set_error(errbuf, errbuf_len, "AOT allocation plan func has no func plan");
    if (!verify_body_summary_anchor(bundle, plan->func, plan->body_func_id, plan->body_effect_bits,
                                    plan->body_escape_bits, plan->body_evidence, "allocation",
                                    errbuf, errbuf_len))
        return false;
    if (plan->value->op != XI_STACK_ALLOC)
        return set_error(errbuf, errbuf_len, "AOT allocation plan value is not stack alloc");
    if (plan->value->aux_int <= 0)
        return set_error(errbuf, errbuf_len, "AOT allocation plan lacks original op");
    original_op = (uint16_t) plan->value->aux_int;
    if (!xi_op_is_heap_alloc(original_op))
        return set_error(errbuf, errbuf_len, "AOT allocation plan original op is not heap alloc");
    if (plan->original_op != original_op)
        return set_error(errbuf, errbuf_len, "AOT allocation plan original op is stale");
    if (plan->escape != plan->value->escape)
        return set_error(errbuf, errbuf_len, "AOT allocation plan escape is stale");
    if (plan->escape != (uint8_t) XI_ESC_NONE)
        return set_error(errbuf, errbuf_len, "AOT allocation plan stack value is escaping");
    if (plan->action != XAOT_ALLOC_ACTION_STACK)
        return set_error(errbuf, errbuf_len, "AOT allocation plan action does not re-derive");
    expected_evidence = verify_allocation_evidence_for(plan->value, original_op);
    if (plan->evidence != expected_evidence)
        return set_error(errbuf, errbuf_len, "AOT allocation plan evidence is stale");
    return true;
}

static bool verify_closure_plan(const XaotBundle *bundle, const XaotClosurePlan *plan, char *errbuf,
                                size_t errbuf_len) {
    XaotClosurePlan derived;

    if (!bundle || !plan)
        return set_error(errbuf, errbuf_len, "AOT closure plan is NULL");
    if (!plan->func || !plan->value)
        return set_error(errbuf, errbuf_len, "AOT closure plan lacks func or value");
    if (!xaot_bundle_find_func_plan(bundle, plan->func))
        return set_error(errbuf, errbuf_len, "AOT closure plan func has no func plan");
    if (!xaot_bundle_find_value_plan(bundle, plan->value))
        return set_error(errbuf, errbuf_len, "AOT closure plan value has no value plan");
    if (xaot_bundle_find_closure_plan(bundle, plan->value) != plan)
        return set_error(errbuf, errbuf_len, "AOT closure plan index mismatch");
    if (!xaot_prepare_closure_plan_for_value(plan->func, plan->value, &derived))
        return set_error(errbuf, errbuf_len, "AOT closure plan value no longer re-derives");
    if (plan->target_func != derived.target_func)
        return set_error(errbuf, errbuf_len, "AOT closure plan target does not re-derive");
    if (plan->capture_count != derived.capture_count)
        return set_error(errbuf, errbuf_len, "AOT closure plan capture count does not re-derive");
    if (plan->representation != derived.representation)
        return set_error(errbuf, errbuf_len, "AOT closure plan representation does not re-derive");
    if (plan->evidence != derived.evidence)
        return set_error(errbuf, errbuf_len, "AOT closure plan evidence does not re-derive");
    if (plan->unproven_reason != derived.unproven_reason)
        return set_error(errbuf, errbuf_len, "AOT closure plan reason does not re-derive");
    return true;
}

static bool verify_transfer_plan(const XaotBundle *bundle, const XaotTransferPlan *plan,
                                 char *errbuf, size_t errbuf_len) {
    XaotTransferPlan derived;

    if (!bundle || !plan)
        return set_error(errbuf, errbuf_len, "AOT transfer plan is NULL");
    if (!plan->func || !plan->site)
        return set_error(errbuf, errbuf_len, "AOT transfer plan lacks func or site");
    if (!xaot_bundle_find_func_plan(bundle, plan->func))
        return set_error(errbuf, errbuf_len, "AOT transfer plan func has no func plan");
    if (!xaot_bundle_find_value_plan(bundle, plan->site))
        return set_error(errbuf, errbuf_len, "AOT transfer plan site has no value plan");
    if (plan->value && !xaot_bundle_find_value_plan(bundle, plan->value))
        return set_error(errbuf, errbuf_len, "AOT transfer plan value has no value plan");
    if (xaot_bundle_find_transfer_plan(bundle, plan->site, plan->transfer_index) != plan)
        return set_error(errbuf, errbuf_len, "AOT transfer plan index mismatch");
    if (!xaot_prepare_transfer_plan_for_site(plan->func, plan->site, plan->transfer_index,
                                             &derived))
        return set_error(errbuf, errbuf_len, "AOT transfer plan site no longer re-derives");
    if (plan->value != derived.value)
        return set_error(errbuf, errbuf_len, "AOT transfer plan value does not re-derive");
    if (plan->value_type != derived.value_type)
        return set_error(errbuf, errbuf_len, "AOT transfer plan value type does not re-derive");
    if (memcmp(&plan->value_type_key, &derived.value_type_key, sizeof(plan->value_type_key)) != 0)
        return set_error(errbuf, errbuf_len, "AOT transfer plan type key does not re-derive");
    if (plan->site_kind != derived.site_kind)
        return set_error(errbuf, errbuf_len, "AOT transfer plan site kind does not re-derive");
    if (plan->mode != derived.mode)
        return set_error(errbuf, errbuf_len, "AOT transfer plan mode does not re-derive");
    if (plan->action != derived.action)
        return set_error(errbuf, errbuf_len, "AOT transfer plan action does not re-derive");
    if (plan->evidence != derived.evidence)
        return set_error(errbuf, errbuf_len, "AOT transfer plan evidence does not re-derive");
    if (plan->unproven_reason != derived.unproven_reason)
        return set_error(errbuf, errbuf_len, "AOT transfer plan reason does not re-derive");
    return true;
}

static bool xg_verify_class_is_runtime_class(const XgClassSummary *cls) {
    return cls && (cls->decl_kind == 0 || cls->decl_kind == XG_DECL_CLASS);
}

static const XgClassSummary *verify_find_evidence_class(const XgGlobalEvidence *ev,
                                                        XgClassId class_id);

static bool xg_verify_class_has_subclass(const XgGlobalEvidence *ev, XgClassId class_id) {
    if (!ev || class_id == XG_NO_ID)
        return false;
    for (uint32_t i = 0; i < ev->nclasses; i++) {
        const XgClassSummary *candidate = &ev->classes[i];
        if (!xg_verify_class_is_runtime_class(candidate))
            continue;
        if (candidate->parent_class_id == class_id)
            return true;
    }
    return false;
}

static bool xg_verify_class_is_descendant_of(const XgGlobalEvidence *ev, XgClassId class_id,
                                             XgClassId ancestor_id) {
    const XgClassSummary *cls;
    uint32_t depth = 0;

    if (!ev || class_id == XG_NO_ID || ancestor_id == XG_NO_ID || class_id == ancestor_id)
        return false;
    cls = verify_find_evidence_class(ev, class_id);
    while (cls && cls->parent_class_id != XG_NO_ID && depth++ < 64) {
        if (cls->parent_class_id == ancestor_id)
            return true;
        cls = verify_find_evidence_class(ev, cls->parent_class_id);
    }
    return false;
}

static bool xg_verify_class_is_descendant_or_self(const XgGlobalEvidence *ev, XgClassId class_id,
                                                  XgClassId ancestor_id) {
    return class_id == ancestor_id || xg_verify_class_is_descendant_of(ev, class_id, ancestor_id);
}

static bool xg_verify_method_participates_in_override(const XgMethodSummary *method) {
    return method && (method->flags & XG_METHOD_STATIC) == 0 &&
           (method->flags & XG_METHOD_CONSTRUCTOR) == 0;
}

static const XgCallsiteSummary *verify_find_evidence_callsite(const XgGlobalEvidence *ev,
                                                              XgCallsiteId callsite_id) {
    if (!ev || callsite_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < ev->ncallsites; i++) {
        if (ev->callsites[i].callsite_id == callsite_id)
            return &ev->callsites[i];
    }
    return NULL;
}

static const XgBodySummary *verify_find_evidence_body_by_func(const XgGlobalEvidence *ev,
                                                              XgFuncId func_id) {
    if (!ev || func_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < ev->nbodies; i++) {
        if (ev->bodies[i].func_id == func_id)
            return &ev->bodies[i];
    }
    return NULL;
}

static XgFuncId verify_find_method_body_func_id(const XgGlobalEvidence *ev, XgMethodId method_id) {
    XgFuncId match = XG_NO_ID;
    if (!ev || method_id == XG_NO_ID)
        return XG_NO_ID;
    for (uint32_t i = 0; i < ev->nbodies; i++) {
        const XgBodySummary *body = &ev->bodies[i];
        if (body->kind != XG_BODY_METHOD || body->owner_method_id != method_id)
            continue;
        if (match != XG_NO_ID && match != body->func_id)
            return XG_NO_ID;
        match = body->func_id;
    }
    return match;
}

static bool verify_body_summary_anchor(const XaotBundle *bundle, const XiFunc *func,
                                       XgFuncId body_func_id, uint32_t body_effect_bits,
                                       uint32_t body_escape_bits, uint32_t body_evidence,
                                       const char *plan_name, char *errbuf, size_t errbuf_len) {
    const XgGlobalEvidence *ev = bundle ? bundle->global_evidence_plan.evidence : NULL;
    const XgBodySummary *body;

    if (body_evidence != XAOT_PLAN_BODY_EV_BODY_SUMMARY) {
        if (errbuf && errbuf_len > 0)
            snprintf(errbuf, errbuf_len, "AOT %s plan lacks body summary evidence", plan_name);
        return false;
    }
    body = verify_find_evidence_body_by_func(ev, body_func_id);
    if (!body) {
        if (errbuf && errbuf_len > 0)
            snprintf(errbuf, errbuf_len, "AOT %s plan has no body summary", plan_name);
        return false;
    }
    if (func && func->name && body->kind != XG_BODY_MODULE_INIT &&
        body->name_id != xg_name_id(func->name)) {
        if (errbuf && errbuf_len > 0)
            snprintf(errbuf, errbuf_len, "AOT %s plan body identity is stale", plan_name);
        return false;
    }
    if (body_effect_bits != body->effect_bits) {
        if (errbuf && errbuf_len > 0)
            snprintf(errbuf, errbuf_len, "AOT %s plan body effect bits are stale", plan_name);
        return false;
    }
    if (body_escape_bits != body->escape_bits) {
        if (errbuf && errbuf_len > 0)
            snprintf(errbuf, errbuf_len, "AOT %s plan body escape bits are stale", plan_name);
        return false;
    }
    return true;
}

static const XgDeclSummary *verify_find_evidence_decl(const XgGlobalEvidence *ev,
                                                      XgDeclId decl_id) {
    if (!ev || decl_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < ev->ndecls; i++) {
        if (ev->decls[i].decl_id == decl_id)
            return &ev->decls[i];
    }
    return NULL;
}

static const XgDeclSummary *verify_find_evidence_func_decl_by_name_flags(const XgGlobalEvidence *ev,
                                                                         uint32_t name_id,
                                                                         uint32_t required_flags) {
    if (!ev || name_id == 0)
        return NULL;
    for (uint32_t i = 0; i < ev->ndecls; i++) {
        const XgDeclSummary *decl = &ev->decls[i];
        if (decl->kind == XG_DECL_FUNC && decl->name_id == name_id &&
            (decl->flags & required_flags) == required_flags)
            return decl;
    }
    return NULL;
}

static const XgDeclSummary *verify_find_evidence_decl_by_kind_name(const XgGlobalEvidence *ev,
                                                                   uint8_t kind, uint32_t name_id) {
    if (!ev || name_id == 0)
        return NULL;
    for (uint32_t i = 0; i < ev->ndecls; i++) {
        const XgDeclSummary *decl = &ev->decls[i];
        if (decl->kind == kind && decl->name_id == name_id)
            return decl;
    }
    return NULL;
}

static const XgInterfaceMethodSummary *
verify_find_evidence_interface_method_depth(const XgGlobalEvidence *ev, XgInterfaceId interface_id,
                                            uint32_t name_id, uint32_t signature_key,
                                            uint32_t depth) {
    if (!ev || interface_id == XG_NO_ID || name_id == 0 || signature_key == 0)
        return NULL;
    if (depth > 64)
        return NULL;
    for (uint32_t i = 0; i < ev->ninterface_methods; i++) {
        const XgInterfaceMethodSummary *method = &ev->interface_methods[i];
        if (method->owner_interface_id == interface_id && method->name_id == name_id &&
            method->signature_key == signature_key)
            return method;
    }
    for (uint32_t i = 0; i < ev->ninterface_extends; i++) {
        const XgInterfaceExtendsSummary *edge = &ev->interface_extends[i];
        const XgInterfaceMethodSummary *method;
        if (edge->child_interface_id != interface_id)
            continue;
        method = verify_find_evidence_interface_method_depth(ev, edge->parent_interface_id, name_id,
                                                             signature_key, depth + 1);
        if (method)
            return method;
    }
    return NULL;
}

static const XgInterfaceMethodSummary *
verify_find_evidence_interface_method(const XgGlobalEvidence *ev, XgInterfaceId interface_id,
                                      uint32_t name_id, uint32_t signature_key) {
    return verify_find_evidence_interface_method_depth(ev, interface_id, name_id, signature_key, 0);
}

static const XgClassSummary *verify_find_evidence_class(const XgGlobalEvidence *ev,
                                                        XgClassId class_id) {
    if (!ev || class_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < ev->nclasses; i++) {
        if (ev->classes[i].class_id == class_id)
            return &ev->classes[i];
    }
    return NULL;
}

static const XgMethodSummary *verify_find_evidence_method_by_id(const XgGlobalEvidence *ev,
                                                                XgMethodId method_id) {
    if (!ev || method_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < ev->nmethods; i++) {
        if (ev->methods[i].method_id == method_id)
            return &ev->methods[i];
    }
    return NULL;
}

static bool verify_generic_inst_anchors_monomorphized_class(const XgGenericInstSummary *inst,
                                                            const XgClassSummary *cls) {
    if (!inst || !cls || inst->specialized_class_id != cls->class_id)
        return false;
    if (inst->kind != XG_GENERIC_INST_CLASS)
        return false;
    if ((inst->flags & XG_GENERIC_INST_CONCRETE_STORAGE) == 0)
        return false;
    return inst->origin_class_id == cls->generic_origin_class_id &&
           inst->name_id == cls->generic_origin_name_id &&
           inst->type_key == cls->generic_type_key &&
           inst->type_arg_key_start == cls->generic_type_arg_key_start &&
           inst->type_arg_count == cls->generic_type_arg_count;
}

static bool verify_monomorphized_class_has_generic_inst_anchor(const XgGlobalEvidence *ev,
                                                               const XgClassSummary *cls) {
    if (!ev || !cls || (cls->flags & XG_CLASS_MONOMORPHIZED) == 0)
        return false;
    for (uint32_t i = 0; i < ev->ngeneric_insts; i++) {
        if (verify_generic_inst_anchors_monomorphized_class(&ev->generic_insts[i], cls))
            return true;
    }
    return false;
}

static bool verify_generic_inst_rows(const XgGlobalEvidence *ev, char *errbuf, size_t errbuf_len) {
    if (!ev)
        return set_error(errbuf, errbuf_len,
                         "AOT global evidence generic inst verifier has no evidence");
    for (uint32_t i = 0; i < ev->ngeneric_insts; i++) {
        const XgGenericInstSummary *inst = &ev->generic_insts[i];
        if (inst->generic_inst_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT generic inst evidence has no id");
        for (uint32_t j = i + 1; j < ev->ngeneric_insts; j++) {
            if (ev->generic_insts[j].generic_inst_id == inst->generic_inst_id)
                return set_error(errbuf, errbuf_len, "AOT generic inst evidence id is duplicated");
            if (inst->specialized_func_id != XG_NO_ID &&
                ev->generic_insts[j].specialized_func_id == inst->specialized_func_id)
                return set_error(errbuf, errbuf_len,
                                 "AOT generic inst specialized body anchor is duplicated");
            if (inst->specialized_class_id != XG_NO_ID &&
                ev->generic_insts[j].specialized_class_id == inst->specialized_class_id)
                return set_error(errbuf, errbuf_len,
                                 "AOT generic inst specialized class anchor is duplicated");
        }
        if (inst->module_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT generic inst evidence has no module");
        switch ((XgGenericInstKind) inst->kind) {
            case XG_GENERIC_INST_FUNCTION:
            case XG_GENERIC_INST_METHOD:
            case XG_GENERIC_INST_CLASS:
            case XG_GENERIC_INST_CONTAINER:
                break;
            default:
                return set_error(errbuf, errbuf_len, "AOT generic inst evidence has invalid kind");
        }
        if (inst->origin_decl_id != XG_NO_ID &&
            !verify_find_evidence_decl(ev, inst->origin_decl_id))
            return set_error(errbuf, errbuf_len, "AOT generic inst origin declaration is missing");
        if (inst->origin_func_id != XG_NO_ID &&
            !verify_find_evidence_body_by_func(ev, inst->origin_func_id))
            return set_error(errbuf, errbuf_len, "AOT generic inst origin body is missing");
        if (inst->origin_method_id != XG_NO_ID &&
            !verify_find_evidence_method_by_id(ev, inst->origin_method_id))
            return set_error(errbuf, errbuf_len, "AOT generic inst origin method is missing");
        if (inst->origin_class_id != XG_NO_ID &&
            !verify_find_evidence_class(ev, inst->origin_class_id))
            return set_error(errbuf, errbuf_len, "AOT generic inst origin class is missing");
        if (inst->specialized_func_id != XG_NO_ID &&
            !verify_find_evidence_body_by_func(ev, inst->specialized_func_id))
            return set_error(errbuf, errbuf_len, "AOT generic inst specialized body is missing");
        if (inst->specialized_class_id != XG_NO_ID &&
            !verify_find_evidence_class(ev, inst->specialized_class_id))
            return set_error(errbuf, errbuf_len, "AOT generic inst specialized class is missing");
        if (inst->specialized_class_id != XG_NO_ID) {
            const XgClassSummary *specialized_class =
                verify_find_evidence_class(ev, inst->specialized_class_id);
            if ((specialized_class->flags & XG_CLASS_MONOMORPHIZED) == 0)
                return set_error(errbuf, errbuf_len,
                                 "AOT generic inst specialized class is not monomorphized");
            if (inst->origin_class_id != XG_NO_ID &&
                specialized_class->generic_origin_class_id != inst->origin_class_id)
                return set_error(errbuf, errbuf_len,
                                 "AOT generic inst specialized class origin does not re-derive");
            if (specialized_class->generic_origin_name_id != inst->name_id ||
                specialized_class->generic_type_key != inst->type_key ||
                specialized_class->generic_type_arg_key_start != inst->type_arg_key_start ||
                specialized_class->generic_type_arg_count != inst->type_arg_count)
                return set_error(errbuf, errbuf_len,
                                 "AOT generic inst specialized class identity does not re-derive");
        }
        if (inst->root_callsite_id != XG_NO_ID &&
            !verify_find_evidence_callsite(ev, inst->root_callsite_id))
            return set_error(errbuf, errbuf_len, "AOT generic inst root callsite is missing");
        if ((inst->flags & XG_GENERIC_INST_CONCRETE_TYPES) != 0 &&
            (inst->type_key == 0 || inst->type_arg_count == 0))
            return set_error(errbuf, errbuf_len,
                             "AOT generic inst concrete type evidence is incomplete");
        if ((inst->flags & XG_GENERIC_INST_INTERFACE_CONSTRAINT) != 0 &&
            inst->constraint_interface_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len,
                             "AOT generic inst interface constraint is missing");
        if ((inst->flags & XG_GENERIC_INST_SPECIALIZED_BODY) != 0 &&
            inst->specialized_func_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT generic inst specialized body is missing");
        if ((inst->flags & XG_GENERIC_INST_SPECIALIZED_ABI) != 0 &&
            inst->specialized_func_id == XG_NO_ID && inst->specialized_class_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len,
                             "AOT generic inst specialized ABI target is missing");
    }
    return true;
}

static bool verify_generic_storage_kind_valid(uint8_t kind) {
    switch ((XgGenericStorageKind) kind) {
        case XG_GENERIC_STORAGE_ARRAY:
        case XG_GENERIC_STORAGE_MAP:
        case XG_GENERIC_STORAGE_SET:
        case XG_GENERIC_STORAGE_CLASS:
        case XG_GENERIC_STORAGE_STRUCT:
            return true;
        default:
            return false;
    }
}

static bool verify_generic_deepen_rows(const XgGlobalEvidence *ev, char *errbuf,
                                       size_t errbuf_len) {
    if (!ev)
        return set_error(errbuf, errbuf_len,
                         "AOT global evidence generic deepen verifier has no evidence");
    for (uint32_t i = 0; i < ev->ngeneric_body_uses; i++) {
        const XgGenericBodyUseSummary *use = &ev->generic_body_uses[i];
        if (use->use_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT generic body-use evidence has no id");
        if (!xg_global_evidence_find_generic_inst(ev, use->generic_inst_id))
            return set_error(errbuf, errbuf_len, "AOT generic body-use references missing inst");
        if (use->module_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT generic body-use evidence has no module");
        if (use->origin_body_func_id != XG_NO_ID &&
            !verify_find_evidence_body_by_func(ev, use->origin_body_func_id))
            return set_error(errbuf, errbuf_len, "AOT generic body-use origin body is missing");
        if (use->specialized_body_func_id != XG_NO_ID &&
            !verify_find_evidence_body_by_func(ev, use->specialized_body_func_id))
            return set_error(errbuf, errbuf_len,
                             "AOT generic body-use specialized body is missing");
        if (use->root_callsite_id != XG_NO_ID &&
            !verify_find_evidence_callsite(ev, use->root_callsite_id))
            return set_error(errbuf, errbuf_len, "AOT generic body-use root callsite is missing");
        for (uint32_t j = i + 1; j < ev->ngeneric_body_uses; j++) {
            if (ev->generic_body_uses[j].use_id == use->use_id)
                return set_error(errbuf, errbuf_len, "AOT generic body-use id is duplicated");
        }
    }
    for (uint32_t i = 0; i < ev->ngeneric_storages; i++) {
        const XgGenericStorageSummary *storage = &ev->generic_storages[i];
        if (storage->storage_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT generic storage evidence has no id");
        if (!xg_global_evidence_find_generic_inst(ev, storage->generic_inst_id))
            return set_error(errbuf, errbuf_len, "AOT generic storage references missing inst");
        if (storage->module_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT generic storage evidence has no module");
        if (!verify_generic_storage_kind_valid(storage->storage_kind))
            return set_error(errbuf, errbuf_len, "AOT generic storage evidence has invalid kind");
        if (storage->origin_type_key == 0)
            return set_error(errbuf, errbuf_len, "AOT generic storage evidence has no origin type");
        for (uint32_t j = i + 1; j < ev->ngeneric_storages; j++) {
            if (ev->generic_storages[j].storage_id == storage->storage_id)
                return set_error(errbuf, errbuf_len, "AOT generic storage id is duplicated");
        }
    }
    for (uint32_t i = 0; i < ev->ngeneric_code_sizes; i++) {
        const XgGenericCodeSizeSummary *size = &ev->generic_code_sizes[i];
        if (size->code_size_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT generic code-size evidence has no id");
        if (!xg_global_evidence_find_generic_inst(ev, size->generic_inst_id))
            return set_error(errbuf, errbuf_len, "AOT generic code-size references missing inst");
        if (size->module_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT generic code-size evidence has no module");
        if (size->body_use_id != XG_NO_ID &&
            !xg_global_evidence_find_generic_body_use(ev, size->body_use_id))
            return set_error(errbuf, errbuf_len,
                             "AOT generic code-size references missing body-use");
        if (size->threshold == 0)
            return set_error(errbuf, errbuf_len, "AOT generic code-size evidence has no threshold");
        for (uint32_t j = i + 1; j < ev->ngeneric_code_sizes; j++) {
            if (ev->generic_code_sizes[j].code_size_id == size->code_size_id)
                return set_error(errbuf, errbuf_len, "AOT generic code-size id is duplicated");
        }
    }
    return true;
}

static uint32_t verify_derive_kind_flag(uint8_t kind) {
    switch ((XgDeriveKind) kind) {
        case XG_DERIVE_JSON:
            return XR_DERIVE_JSON;
        case XG_DERIVE_INSPECT:
            return XR_DERIVE_INSPECT;
        case XG_DERIVE_EQ:
            return XR_DERIVE_EQ;
        case XG_DERIVE_HASH:
            return XR_DERIVE_HASH;
        case XG_DERIVE_CLONE:
            return XR_DERIVE_CLONE;
        default:
            return 0;
    }
}

static const XgDeriveSummary *verify_find_derive_by_id(const XgGlobalEvidence *ev,
                                                       XgDeriveId derive_id) {
    if (!ev || derive_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < ev->nderives; i++) {
        if (ev->derives[i].derive_id == derive_id)
            return &ev->derives[i];
    }
    return NULL;
}

static bool verify_decl_has_derive_row(const XgGlobalEvidence *ev, XgDeclId decl_id,
                                       uint8_t derive_kind) {
    if (!ev || decl_id == XG_NO_ID)
        return false;
    for (uint32_t i = 0; i < ev->nderives; i++) {
        const XgDeriveSummary *derive = &ev->derives[i];
        if (derive->owner_decl_id == decl_id && derive->derive_kind == derive_kind)
            return true;
    }
    return false;
}

static bool verify_derive_rows(const XgGlobalEvidence *ev, char *errbuf, size_t errbuf_len) {
    if (!ev)
        return set_error(errbuf, errbuf_len, "AOT global evidence derive verifier has no evidence");
    for (uint32_t i = 0; i < ev->nderives; i++) {
        const XgDeriveSummary *derive = &ev->derives[i];
        const XgDeclSummary *decl;
        uint32_t expected_flag;
        if (derive->derive_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT derive evidence has no id");
        for (uint32_t j = i + 1; j < ev->nderives; j++) {
            if (ev->derives[j].derive_id == derive->derive_id)
                return set_error(errbuf, errbuf_len, "AOT derive evidence id is duplicated");
            if (ev->derives[j].owner_decl_id == derive->owner_decl_id &&
                ev->derives[j].derive_kind == derive->derive_kind)
                return set_error(errbuf, errbuf_len, "AOT derive evidence kind is duplicated");
        }
        if (derive->module_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT derive evidence has no module");
        decl = verify_find_evidence_decl(ev, derive->owner_decl_id);
        if (!decl)
            return set_error(errbuf, errbuf_len, "AOT derive owner declaration is missing");
        expected_flag = verify_derive_kind_flag(derive->derive_kind);
        if (expected_flag == 0)
            return set_error(errbuf, errbuf_len, "AOT derive evidence has invalid kind");
        if ((decl->derive_flags & expected_flag) == 0)
            return set_error(errbuf, errbuf_len, "AOT derive evidence kind does not re-derive");
        if ((decl->flags & XG_DECL_DERIVE) == 0)
            return set_error(errbuf, errbuf_len, "AOT derive evidence owner lacks derive flag");
        if (derive->type_key == 0)
            return set_error(errbuf, errbuf_len, "AOT derive evidence has no type key");
        if (derive->field_count == 0 && derive->field_start != 0)
            return set_error(errbuf, errbuf_len, "AOT derive empty field range is stale");
        if (derive->field_count != 0) {
            uint32_t start = derive->field_start;
            uint32_t end = start + derive->field_count - 1;
            if (start == 0 || end < start || end > ev->nderived_fields)
                return set_error(errbuf, errbuf_len, "AOT derive field range is stale");
            for (uint32_t fi = 0; fi < derive->field_count; fi++) {
                const XgDerivedFieldSummary *field = &ev->derived_fields[start - 1 + fi];
                if (field->derive_id != derive->derive_id)
                    return set_error(errbuf, errbuf_len,
                                     "AOT derive field range owner does not re-derive");
                if (field->field_ordinal != fi)
                    return set_error(errbuf, errbuf_len,
                                     "AOT derive field ordinal does not re-derive");
            }
        }
        if (derive->method_count == 0 && derive->method_start != 0)
            return set_error(errbuf, errbuf_len, "AOT derive empty method range is stale");
        if (derive->method_count != 0) {
            uint32_t start = derive->method_start;
            uint32_t end = start + derive->method_count - 1;
            if (start == 0 || end < start || end > ev->nderived_methods)
                return set_error(errbuf, errbuf_len, "AOT derive method range is stale");
            for (uint32_t mi = 0; mi < derive->method_count; mi++) {
                const XgDerivedMethodSummary *method = &ev->derived_methods[start - 1 + mi];
                if (method->derive_id != derive->derive_id)
                    return set_error(errbuf, errbuf_len,
                                     "AOT derive method range owner does not re-derive");
            }
        }
    }
    for (uint32_t i = 0; i < ev->nderived_fields; i++) {
        const XgDerivedFieldSummary *field = &ev->derived_fields[i];
        if (field->field_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT derived field evidence has no id");
        for (uint32_t j = i + 1; j < ev->nderived_fields; j++) {
            if (ev->derived_fields[j].field_id == field->field_id)
                return set_error(errbuf, errbuf_len, "AOT derived field evidence id is duplicated");
        }
        if (!verify_find_derive_by_id(ev, field->derive_id))
            return set_error(errbuf, errbuf_len, "AOT derived field owner derive is missing");
    }
    for (uint32_t i = 0; i < ev->nderived_methods; i++) {
        const XgDerivedMethodSummary *method = &ev->derived_methods[i];
        if (method->method_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT derived method evidence has no id");
        for (uint32_t j = i + 1; j < ev->nderived_methods; j++) {
            if (ev->derived_methods[j].method_id == method->method_id)
                return set_error(errbuf, errbuf_len,
                                 "AOT derived method evidence id is duplicated");
        }
        if (!verify_find_derive_by_id(ev, method->derive_id))
            return set_error(errbuf, errbuf_len, "AOT derived method owner derive is missing");
        switch ((XgDerivedMethodKind) method->method_kind) {
            case XG_DERIVED_METHOD_JSON_ENCODE:
            case XG_DERIVED_METHOD_INSPECT_FORMAT:
            case XG_DERIVED_METHOD_EQ:
            case XG_DERIVED_METHOD_HASH:
            case XG_DERIVED_METHOD_CLONE:
                break;
            default:
                return set_error(errbuf, errbuf_len,
                                 "AOT derived method evidence has invalid kind");
        }
    }
    for (uint32_t i = 0; i < ev->ndecls; i++) {
        const XgDeclSummary *decl = &ev->decls[i];
        if ((decl->derive_flags & XR_DERIVE_JSON) != 0 &&
            !verify_decl_has_derive_row(ev, decl->decl_id, XG_DERIVE_JSON))
            return set_error(errbuf, errbuf_len, "AOT Json derive row is missing");
        if ((decl->derive_flags & XR_DERIVE_INSPECT) != 0 &&
            !verify_decl_has_derive_row(ev, decl->decl_id, XG_DERIVE_INSPECT))
            return set_error(errbuf, errbuf_len, "AOT Inspect derive row is missing");
        if ((decl->derive_flags & XR_DERIVE_EQ) != 0 &&
            !verify_decl_has_derive_row(ev, decl->decl_id, XG_DERIVE_EQ))
            return set_error(errbuf, errbuf_len, "AOT Eq derive row is missing");
        if ((decl->derive_flags & XR_DERIVE_HASH) != 0 &&
            !verify_decl_has_derive_row(ev, decl->decl_id, XG_DERIVE_HASH))
            return set_error(errbuf, errbuf_len, "AOT Hash derive row is missing");
        if ((decl->derive_flags & XR_DERIVE_CLONE) != 0 &&
            !verify_decl_has_derive_row(ev, decl->decl_id, XG_DERIVE_CLONE))
            return set_error(errbuf, errbuf_len, "AOT Clone derive row is missing");
    }
    return true;
}

static bool verify_json_shape_kind_valid(uint8_t kind) {
    switch ((XgJsonShapeKind) kind) {
        case XG_JSON_SHAPE_OPEN:
        case XG_JSON_SHAPE_SHAPED:
        case XG_JSON_SHAPE_RECORD_BRIDGE:
            return true;
        default:
            return false;
    }
}

static bool verify_json_access_kind_valid(uint8_t kind) {
    switch ((XgJsonAccessKind) kind) {
        case XG_JSON_ACCESS_FIELD_GET:
        case XG_JSON_ACCESS_FIELD_SET:
        case XG_JSON_ACCESS_INDEX_GET:
        case XG_JSON_ACCESS_INDEX_SET:
        case XG_JSON_ACCESS_GET_DEFAULT:
            return true;
        default:
            return false;
    }
}

static bool verify_json_codec_row_kind_valid(uint8_t kind) {
    switch ((XgJsonCodecKind) kind) {
        case XG_JSON_CODEC_PARSE:
        case XG_JSON_CODEC_DECODE:
        case XG_JSON_CODEC_ENCODE:
        case XG_JSON_CODEC_STRINGIFY:
            return true;
        default:
            return false;
    }
}

static bool verify_json_rows(const XgGlobalEvidence *ev, char *errbuf, size_t errbuf_len) {
    if (!ev)
        return set_error(errbuf, errbuf_len, "AOT global evidence Json verifier has no evidence");
    for (uint32_t i = 0; i < ev->njson_shapes; i++) {
        const XgJsonShapeSummary *shape = &ev->json_shapes[i];
        if (shape->json_shape_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT Json shape evidence has no id");
        if (!verify_json_shape_kind_valid(shape->shape_kind))
            return set_error(errbuf, errbuf_len, "AOT Json shape evidence has invalid kind");
        for (uint32_t j = i + 1; j < ev->njson_shapes; j++) {
            if (ev->json_shapes[j].json_shape_id == shape->json_shape_id)
                return set_error(errbuf, errbuf_len, "AOT Json shape evidence id is duplicated");
        }
    }
    for (uint32_t i = 0; i < ev->njson_accesses; i++) {
        const XgJsonAccessSummary *access = &ev->json_accesses[i];
        const XgJsonShapeSummary *shape;
        if (access->json_access_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT Json access evidence has no id");
        if (!verify_json_access_kind_valid(access->access_kind))
            return set_error(errbuf, errbuf_len, "AOT Json access evidence has invalid kind");
        for (uint32_t j = i + 1; j < ev->njson_accesses; j++) {
            if (ev->json_accesses[j].json_access_id == access->json_access_id)
                return set_error(errbuf, errbuf_len, "AOT Json access evidence id is duplicated");
        }
        if (access->receiver_shape_id == XG_NO_ID)
            continue;
        shape = xg_global_evidence_find_json_shape(ev, access->receiver_shape_id);
        if (!shape)
            return set_error(errbuf, errbuf_len, "AOT Json access references missing shape");
        if ((access->flags & XG_JSON_ACCESS_COMPUTED_KEY) == 0 && access->key_name_id != 0 &&
            access->field_ordinal >= shape->field_count)
            return set_error(errbuf, errbuf_len, "AOT Json access field ordinal is stale");
    }
    for (uint32_t i = 0; i < ev->njson_codecs; i++) {
        const XgJsonCodecSummary *codec = &ev->json_codecs[i];
        if (codec->codec_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT Json codec evidence has no id");
        if (!verify_json_codec_row_kind_valid(codec->codec_kind))
            return set_error(errbuf, errbuf_len, "AOT Json codec evidence has invalid kind");
        for (uint32_t j = i + 1; j < ev->njson_codecs; j++) {
            if (ev->json_codecs[j].codec_id == codec->codec_id)
                return set_error(errbuf, errbuf_len, "AOT Json codec evidence id is duplicated");
        }
        if ((codec->flags & XG_JSON_CODEC_HAS_INPUT_SHAPE) != 0 &&
            codec->input_shape_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT Json codec input shape evidence is missing");
        if ((codec->flags & XG_JSON_CODEC_HAS_OUTPUT_SHAPE) != 0 &&
            codec->output_shape_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT Json codec output shape evidence is missing");
        if (codec->input_shape_id != XG_NO_ID &&
            !xg_global_evidence_find_json_shape(ev, codec->input_shape_id))
            return set_error(errbuf, errbuf_len, "AOT Json codec references missing input shape");
        if (codec->output_shape_id != XG_NO_ID &&
            !xg_global_evidence_find_json_shape(ev, codec->output_shape_id))
            return set_error(errbuf, errbuf_len, "AOT Json codec references missing output shape");
    }
    return true;
}

static bool verify_record_shape_kind_valid(uint8_t kind) {
    switch ((XgRecordShapeKind) kind) {
        case XG_RECORD_SHAPE_LITERAL:
        case XG_RECORD_SHAPE_OPTIONS:
        case XG_RECORD_SHAPE_SPREAD:
        case XG_RECORD_SHAPE_STATIC:
            return true;
        default:
            return false;
    }
}

static bool verify_record_access_kind_valid(uint8_t kind) {
    switch ((XgRecordAccessKind) kind) {
        case XG_RECORD_ACCESS_FIELD_GET:
        case XG_RECORD_ACCESS_FIELD_SET:
        case XG_RECORD_ACCESS_DESTRUCTURE:
            return true;
        default:
            return false;
    }
}

static bool verify_record_rows(const XgGlobalEvidence *ev, char *errbuf, size_t errbuf_len) {
    if (!ev)
        return set_error(errbuf, errbuf_len, "AOT global evidence Record verifier has no evidence");
    for (uint32_t i = 0; i < ev->nrecord_shapes; i++) {
        const XgRecordShapeSummary *shape = &ev->record_shapes[i];
        if (shape->record_shape_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT Record shape evidence has no id");
        if (!verify_record_shape_kind_valid(shape->shape_kind))
            return set_error(errbuf, errbuf_len, "AOT Record shape evidence has invalid kind");
        for (uint32_t j = i + 1; j < ev->nrecord_shapes; j++) {
            if (ev->record_shapes[j].record_shape_id == shape->record_shape_id)
                return set_error(errbuf, errbuf_len, "AOT Record shape evidence id is duplicated");
        }
    }
    for (uint32_t i = 0; i < ev->nrecord_accesses; i++) {
        const XgRecordAccessSummary *access = &ev->record_accesses[i];
        const XgRecordShapeSummary *shape;
        if (access->record_access_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT Record access evidence has no id");
        if (!verify_record_access_kind_valid(access->access_kind))
            return set_error(errbuf, errbuf_len, "AOT Record access evidence has invalid kind");
        for (uint32_t j = i + 1; j < ev->nrecord_accesses; j++) {
            if (ev->record_accesses[j].record_access_id == access->record_access_id)
                return set_error(errbuf, errbuf_len, "AOT Record access evidence id is duplicated");
        }
        if (access->receiver_shape_id == XG_NO_ID)
            continue;
        shape = xg_global_evidence_find_record_shape(ev, access->receiver_shape_id);
        if (!shape)
            return set_error(errbuf, errbuf_len, "AOT Record access references missing shape");
        if ((access->flags & XG_RECORD_ACCESS_STATIC_FIELD) != 0 && access->field_name_id != 0 &&
            access->field_ordinal >= shape->field_count)
            return set_error(errbuf, errbuf_len, "AOT Record access field ordinal is stale");
    }
    return true;
}

static bool verify_map_container_kind_valid(uint8_t kind) {
    return kind == XG_MAP_CONTAINER_MAP || kind == XG_MAP_CONTAINER_SET;
}

static bool verify_map_shape_source_valid(uint8_t source) {
    switch ((XgMapShapeSource) source) {
        case XG_MAP_SHAPE_SRC_LITERAL:
        case XG_MAP_SHAPE_SRC_CONSTRUCTOR:
        case XG_MAP_SHAPE_SRC_FROM_ARRAY:
        case XG_MAP_SHAPE_SRC_STATIC:
            return true;
        default:
            return false;
    }
}

static bool verify_key_access_op_valid(uint8_t op) {
    switch ((XgKeyAccessOp) op) {
        case XG_KEY_ACCESS_GET:
        case XG_KEY_ACCESS_INDEX_GET:
        case XG_KEY_ACCESS_SET:
        case XG_KEY_ACCESS_HAS:
        case XG_KEY_ACCESS_DELETE:
        case XG_KEY_ACCESS_ADD:
        case XG_KEY_ACCESS_CLEAR:
            return true;
        default:
            return false;
    }
}

static bool verify_hash_eq_kind_valid(uint8_t kind) {
    switch ((XgHashEqKind) kind) {
        case XG_HASH_EQ_BUILTIN:
        case XG_HASH_EQ_ENUM_ORDINAL:
        case XG_HASH_EQ_DERIVE:
        case XG_HASH_EQ_USER_METHOD:
        case XG_HASH_EQ_MISSING:
            return true;
        default:
            return false;
    }
}

static bool verify_map_shape_value_type_supports_bool_direct(uint32_t value_type_key) {
    return value_type_key == xg_synthetic_type_key(XR_TREF_INT) ||
           value_type_key == xg_synthetic_width_type_key(XR_TREF_INT_WIDTH, XR_TREF_NW_I64) ||
           value_type_key == xg_synthetic_width_type_key(XR_TREF_FLOAT_WIDTH, XR_TREF_NW_F32);
}

static bool verify_map_shape_supports_bool_direct(const XgMapShapeSummary *shape) {
    return shape && shape->container_kind == XG_MAP_CONTAINER_MAP &&
           shape->key_type_key == xg_synthetic_type_key(XR_TREF_BOOL) &&
           verify_map_shape_value_type_supports_bool_direct(shape->value_type_key) &&
           shape->entry_count > 0 && shape->entry_count <= 2;
}

static bool verify_map_rows(const XgGlobalEvidence *ev, char *errbuf, size_t errbuf_len) {
    if (!ev)
        return set_error(errbuf, errbuf_len,
                         "AOT global evidence Map/Set verifier has no evidence");
    for (uint32_t i = 0; i < ev->nmap_shapes; i++) {
        const XgMapShapeSummary *shape = &ev->map_shapes[i];
        if (shape->shape_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT Map/Set shape evidence has no id");
        if (!verify_map_container_kind_valid(shape->container_kind) ||
            !verify_map_shape_source_valid(shape->source))
            return set_error(errbuf, errbuf_len, "AOT Map/Set shape evidence has invalid kind");
        if (shape->key_type_key == 0)
            return set_error(errbuf, errbuf_len, "AOT Map/Set shape evidence has no key type");
        if (shape->container_kind == XG_MAP_CONTAINER_MAP && shape->value_type_key == 0)
            return set_error(errbuf, errbuf_len, "AOT Map shape evidence has no value type");
        if (shape->entry_count == 0 && shape->entry_start != 0)
            return set_error(errbuf, errbuf_len, "AOT Map/Set shape empty entry range is stale");
        if (shape->entry_count != 0) {
            uint32_t start = shape->entry_start;
            uint32_t end = start + shape->entry_count - 1;
            if (start == 0 || end < start || end > ev->nmap_entries)
                return set_error(errbuf, errbuf_len, "AOT Map/Set shape entry range is stale");
            for (uint32_t ei = 0; ei < shape->entry_count; ei++) {
                const XgMapEntrySummary *entry = &ev->map_entries[start - 1 + ei];
                if (entry->shape_id != shape->shape_id)
                    return set_error(errbuf, errbuf_len,
                                     "AOT Map/Set entry range owner does not re-derive");
                if (entry->entry_ordinal != ei)
                    return set_error(errbuf, errbuf_len,
                                     "AOT Map/Set entry ordinal does not re-derive");
            }
            if ((shape->flags & XG_MAP_SHAPE_DENSE_INT) != 0) {
                for (uint32_t ei = 0; ei < shape->entry_count; ei++) {
                    const XgMapEntrySummary *entry = &ev->map_entries[start - 1 + ei];
                    if ((entry->flags & XG_MAP_ENTRY_INT_KEY) == 0)
                        return set_error(errbuf, errbuf_len,
                                         "AOT dense Map/Set shape has non-integer key evidence");
                    if (entry->key_i64 != (int64_t) ei)
                        return set_error(errbuf, errbuf_len,
                                         "AOT dense Map/Set shape key domain does not re-derive");
                    if ((entry->flags & XG_MAP_ENTRY_DUPLICATE_KEY) != 0)
                        return set_error(errbuf, errbuf_len,
                                         "AOT dense Map/Set shape has duplicate key evidence");
                }
            }
            if ((shape->flags & XG_MAP_SHAPE_BOOL_DIRECT) != 0) {
                uint8_t seen = 0;
                if (!verify_map_shape_supports_bool_direct(shape))
                    return set_error(errbuf, errbuf_len,
                                     "AOT bool-direct Map shape storage does not re-derive");
                for (uint32_t ei = 0; ei < shape->entry_count; ei++) {
                    const XgMapEntrySummary *entry = &ev->map_entries[start - 1 + ei];
                    if ((entry->flags & XG_MAP_ENTRY_BOOL_KEY) == 0)
                        return set_error(errbuf, errbuf_len,
                                         "AOT bool-direct Map shape has non-bool key evidence");
                    if (entry->key_i64 != 0 && entry->key_i64 != 1)
                        return set_error(errbuf, errbuf_len,
                                         "AOT bool-direct Map shape key domain is stale");
                    if ((seen & (uint8_t) (1u << entry->key_i64)) != 0 ||
                        (entry->flags & XG_MAP_ENTRY_DUPLICATE_KEY) != 0)
                        return set_error(errbuf, errbuf_len,
                                         "AOT bool-direct Map shape has duplicate key evidence");
                    seen |= (uint8_t) (1u << entry->key_i64);
                }
            }
        }
        for (uint32_t j = i + 1; j < ev->nmap_shapes; j++) {
            if (ev->map_shapes[j].shape_id == shape->shape_id)
                return set_error(errbuf, errbuf_len, "AOT Map/Set shape evidence id is duplicated");
        }
    }
    for (uint32_t i = 0; i < ev->nmap_entries; i++) {
        const XgMapEntrySummary *entry = &ev->map_entries[i];
        if (entry->entry_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT Map/Set entry evidence has no id");
        if (!xg_global_evidence_find_map_shape(ev, entry->shape_id))
            return set_error(errbuf, errbuf_len, "AOT Map/Set entry references missing shape");
        for (uint32_t j = i + 1; j < ev->nmap_entries; j++) {
            if (ev->map_entries[j].entry_id == entry->entry_id)
                return set_error(errbuf, errbuf_len, "AOT Map/Set entry evidence id is duplicated");
        }
    }
    for (uint32_t i = 0; i < ev->nkey_accesses; i++) {
        const XgKeyAccessSummary *access = &ev->key_accesses[i];
        if (access->access_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT key access evidence has no id");
        if (!verify_map_container_kind_valid(access->container_kind) ||
            !verify_key_access_op_valid(access->op))
            return set_error(errbuf, errbuf_len, "AOT key access evidence has invalid kind");
        if (access->key_type_key == 0)
            return set_error(errbuf, errbuf_len, "AOT key access evidence has no key type");
        if (access->receiver_shape_id != XG_NO_ID &&
            !xg_global_evidence_find_map_shape(ev, access->receiver_shape_id))
            return set_error(errbuf, errbuf_len, "AOT key access references missing shape");
        for (uint32_t j = i + 1; j < ev->nkey_accesses; j++) {
            if (ev->key_accesses[j].access_id == access->access_id)
                return set_error(errbuf, errbuf_len, "AOT key access evidence id is duplicated");
        }
    }
    for (uint32_t i = 0; i < ev->nhash_eqs; i++) {
        const XgHashEqSummary *hash_eq = &ev->hash_eqs[i];
        if (hash_eq->hash_eq_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT Hash/Eq evidence has no id");
        if (hash_eq->type_key == 0)
            return set_error(errbuf, errbuf_len, "AOT Hash/Eq evidence has no type");
        if (!verify_hash_eq_kind_valid(hash_eq->kind))
            return set_error(errbuf, errbuf_len, "AOT Hash/Eq evidence has invalid kind");
        for (uint32_t j = i + 1; j < ev->nhash_eqs; j++) {
            if (ev->hash_eqs[j].hash_eq_id == hash_eq->hash_eq_id)
                return set_error(errbuf, errbuf_len, "AOT Hash/Eq evidence id is duplicated");
            if (ev->hash_eqs[j].type_key == hash_eq->type_key)
                return set_error(errbuf, errbuf_len, "AOT Hash/Eq evidence type is duplicated");
        }
    }
    return true;
}

static bool verify_class_method_range_contains(const XgGlobalEvidence *ev,
                                               const XgClassSummary *cls,
                                               const XgMethodSummary *method) {
    if (!ev || !cls || !method || cls->method_start == 0)
        return false;
    for (uint32_t i = 0; i < cls->method_count; i++) {
        uint32_t idx = cls->method_start - 1 + i;
        if (idx >= ev->nmethods)
            return false;
        if (&ev->methods[idx] == method)
            return true;
    }
    return false;
}

static const XgMethodSummary *verify_find_evidence_method_in_class(const XgGlobalEvidence *ev,
                                                                   const XgClassSummary *cls,
                                                                   XgMethodId method_or_name_id) {
    if (!ev || !cls || cls->method_start == 0 || method_or_name_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < cls->method_count; i++) {
        uint32_t idx = cls->method_start - 1 + i;
        const XgMethodSummary *method = idx < ev->nmethods ? &ev->methods[idx] : NULL;
        if (method && (method->method_id == method_or_name_id ||
                       method->name_id == (uint32_t) method_or_name_id))
            return method;
    }
    return NULL;
}

static const XgMethodSummary *
verify_find_evidence_method_by_signature_in_class(const XgGlobalEvidence *ev,
                                                  const XgClassSummary *cls, uint32_t name_id,
                                                  uint32_t signature_key, bool allow_constructor) {
    if (!ev || !cls || cls->method_start == 0 || name_id == 0)
        return NULL;
    for (uint32_t i = 0; i < cls->method_count; i++) {
        uint32_t idx = cls->method_start - 1 + i;
        const XgMethodSummary *method = idx < ev->nmethods ? &ev->methods[idx] : NULL;
        bool is_constructor = method && (method->flags & XG_METHOD_CONSTRUCTOR) != 0;
        if (method && method->owner_class_id == cls->class_id &&
            (method->flags & XG_METHOD_STATIC) == 0 && (allow_constructor || !is_constructor) &&
            method->name_id == name_id && method->signature_key == signature_key)
            return method;
    }
    return NULL;
}

static const XgMethodSummary *
verify_find_evidence_method_in_hierarchy(const XgGlobalEvidence *ev, XgClassId class_id,
                                         XgMethodId method_or_name_id) {
    const XgClassSummary *cls = verify_find_evidence_class(ev, class_id);
    uint32_t depth = 0;
    while (cls && depth++ < 64) {
        const XgMethodSummary *method =
            verify_find_evidence_method_in_class(ev, cls, method_or_name_id);
        if (method)
            return method;
        if (cls->parent_class_id == XG_NO_ID)
            break;
        cls = verify_find_evidence_class(ev, cls->parent_class_id);
    }
    return NULL;
}

static const XgMethodSummary *verify_find_evidence_method_by_signature_in_hierarchy(
    const XgGlobalEvidence *ev, XgClassId class_id, uint32_t name_id, uint32_t signature_key,
    bool allow_constructor) {
    const XgClassSummary *cls = verify_find_evidence_class(ev, class_id);
    uint32_t depth = 0;
    while (cls && depth++ < 64) {
        const XgMethodSummary *method = verify_find_evidence_method_by_signature_in_class(
            ev, cls, name_id, signature_key, allow_constructor);
        if (method)
            return method;
        if (cls->parent_class_id == XG_NO_ID)
            break;
        cls = verify_find_evidence_class(ev, cls->parent_class_id);
    }
    return NULL;
}

static const XgMethodSummary *verify_find_parent_method_by_signature(const XgGlobalEvidence *ev,
                                                                     const XgClassSummary *cls,
                                                                     uint32_t name_id,
                                                                     uint32_t signature_key) {
    XgClassId parent_id;
    uint32_t depth = 0;

    if (!ev || !cls)
        return NULL;
    parent_id = cls->parent_class_id;
    while (parent_id != XG_NO_ID && depth++ < 64) {
        const XgClassSummary *parent = verify_find_evidence_class(ev, parent_id);
        const XgMethodSummary *method;
        if (!parent)
            return NULL;
        method = verify_find_evidence_method_by_signature_in_class(ev, parent, name_id,
                                                                   signature_key, false);
        if (method)
            return method;
        parent_id = parent->parent_class_id;
    }
    return NULL;
}

static bool xg_verify_method_is_overridden(const XgGlobalEvidence *ev,
                                           const XgMethodSummary *method) {
    const XgClassSummary *owner;

    if (!ev || !xg_verify_method_participates_in_override(method))
        return false;
    owner = verify_find_evidence_class(ev, method->owner_class_id);
    if (!owner)
        return false;
    for (uint32_t i = 0; i < ev->nmethods; i++) {
        const XgMethodSummary *candidate = &ev->methods[i];
        const XgClassSummary *candidate_owner;
        const XgMethodSummary *expected_parent;
        if (candidate == method || !xg_verify_method_participates_in_override(candidate))
            continue;
        if (candidate->name_id != method->name_id ||
            candidate->signature_key != method->signature_key)
            continue;
        candidate_owner = verify_find_evidence_class(ev, candidate->owner_class_id);
        if (!candidate_owner ||
            !xg_verify_class_is_descendant_of(ev, candidate_owner->class_id, owner->class_id))
            continue;
        expected_parent = verify_find_parent_method_by_signature(
            ev, candidate_owner, candidate->name_id, candidate->signature_key);
        if (expected_parent && expected_parent->method_id == method->method_id)
            return true;
    }
    return false;
}

static XgMethodId xg_verify_rederive_method_root(const XgGlobalEvidence *ev,
                                                 const XgMethodSummary *method) {
    const XgClassSummary *current;
    XgMethodId root;
    uint32_t depth = 0;

    if (!ev || !method || method->method_id == XG_NO_ID)
        return XG_NO_ID;
    root = method->method_id;
    if (!xg_verify_method_participates_in_override(method))
        return root;
    current = verify_find_evidence_class(ev, method->owner_class_id);
    while (current && current->parent_class_id != XG_NO_ID && depth++ < 64) {
        const XgMethodSummary *parent_method;
        const XgClassSummary *parent_class =
            verify_find_evidence_class(ev, current->parent_class_id);
        if (!parent_class)
            break;
        parent_method = verify_find_evidence_method_by_signature_in_class(
            ev, parent_class, method->name_id, method->signature_key, false);
        if (parent_method)
            root = parent_method->method_id;
        current = parent_class;
    }
    return root;
}

static uint32_t xg_verify_rederive_method_override_depth(const XgGlobalEvidence *ev,
                                                         const XgMethodSummary *method) {
    const XgClassSummary *current;
    uint32_t chain_depth = 0;
    uint32_t scan_depth = 0;

    if (!ev || !method || !xg_verify_method_participates_in_override(method))
        return 0;
    current = verify_find_evidence_class(ev, method->owner_class_id);
    while (current && current->parent_class_id != XG_NO_ID && scan_depth++ < 64) {
        const XgMethodSummary *parent_method;
        const XgClassSummary *parent_class =
            verify_find_evidence_class(ev, current->parent_class_id);
        if (!parent_class)
            break;
        parent_method = verify_find_evidence_method_by_signature_in_class(
            ev, parent_class, method->name_id, method->signature_key, false);
        if (parent_method)
            chain_depth++;
        current = parent_class;
    }
    return chain_depth;
}

static bool verify_method_override_graph(const XgGlobalEvidence *ev, char *errbuf,
                                         size_t errbuf_len) {
    if (!ev)
        return set_error(errbuf, errbuf_len, "AOT global evidence method verifier has no evidence");

    for (uint32_t i = 0; i < ev->nmethods; i++) {
        const XgMethodSummary *method = &ev->methods[i];
        const XgClassSummary *owner;
        const XgMethodSummary *expected_parent = NULL;
        XgMethodId expected_override_of = XG_NO_ID;
        XgMethodId expected_root_method_id = XG_NO_ID;
        uint32_t expected_override_depth = 0;
        bool expected_overridden = false;

        if (method->method_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT global evidence method has no id");
        for (uint32_t j = i + 1; j < ev->nmethods; j++) {
            if (ev->methods[j].method_id == method->method_id)
                return set_error(errbuf, errbuf_len, "AOT global evidence method id is duplicated");
        }
        owner = verify_find_evidence_class(ev, method->owner_class_id);
        if (!owner)
            return set_error(errbuf, errbuf_len, "AOT global evidence method owner is missing");
        if (!verify_class_method_range_contains(ev, owner, method))
            return set_error(errbuf, errbuf_len, "AOT global evidence method owner range is stale");

        if (xg_verify_method_participates_in_override(method)) {
            expected_parent = verify_find_parent_method_by_signature(ev, owner, method->name_id,
                                                                     method->signature_key);
            expected_override_of = expected_parent ? expected_parent->method_id : XG_NO_ID;
            expected_overridden = xg_verify_method_is_overridden(ev, method);
        }
        expected_root_method_id = xg_verify_rederive_method_root(ev, method);
        expected_override_depth = xg_verify_rederive_method_override_depth(ev, method);

        if (method->override_of != expected_override_of)
            return set_error(errbuf, errbuf_len,
                             "AOT global evidence method override_of does not re-derive");
        if (method->root_method_id != expected_root_method_id)
            return set_error(errbuf, errbuf_len,
                             "AOT global evidence method root does not re-derive");
        if (method->override_depth != expected_override_depth)
            return set_error(errbuf, errbuf_len,
                             "AOT global evidence method depth does not re-derive");
        if (((method->flags & XG_METHOD_OVERRIDDEN) != 0) != expected_overridden)
            return set_error(errbuf, errbuf_len,
                             "AOT global evidence method overridden flag does not re-derive");
    }
    return true;
}

static bool verify_class_interface_range_contains(const XgGlobalEvidence *ev,
                                                  const XgClassSummary *cls,
                                                  const XgInterfaceImplSummary *impl) {
    if (!ev || !cls || !impl || cls->interface_start == 0)
        return false;
    for (uint32_t i = 0; i < cls->interface_count; i++) {
        uint32_t idx = cls->interface_start - 1 + i;
        if (idx >= ev->ninterface_impls)
            return false;
        if (&ev->interface_impls[idx] == impl)
            return true;
    }
    return false;
}

static bool verify_interface_implementor_set(const XgGlobalEvidence *ev, const XaotBundle *bundle,
                                             char *errbuf, size_t errbuf_len) {
    if (!ev || !bundle)
        return set_error(errbuf, errbuf_len,
                         "AOT global evidence interface verifier has incomplete input");

    for (uint32_t i = 0; i < ev->nclasses; i++) {
        const XgClassSummary *cls = &ev->classes[i];
        if (cls->interface_count == 0) {
            if (cls->interface_start != 0)
                return set_error(errbuf, errbuf_len,
                                 "AOT global evidence class interface range is stale");
            continue;
        }
        if (cls->interface_start == 0 ||
            cls->interface_start - 1 + cls->interface_count > ev->ninterface_impls)
            return set_error(errbuf, errbuf_len,
                             "AOT global evidence class interface range is stale");
        for (uint32_t j = 0; j < cls->interface_count; j++) {
            uint32_t idx = cls->interface_start - 1 + j;
            const XgInterfaceImplSummary *impl = &ev->interface_impls[idx];
            if (impl->implementor_class_id != cls->class_id)
                return set_error(errbuf, errbuf_len,
                                 "AOT global evidence class interface range does not re-derive");
            if (impl->interface_id == XG_NO_ID || impl->name_id == 0)
                return set_error(errbuf, errbuf_len,
                                 "AOT global evidence interface impl has no interface");
            for (uint32_t k = j + 1; k < cls->interface_count; k++) {
                const XgInterfaceImplSummary *other =
                    &ev->interface_impls[cls->interface_start - 1 + k];
                if (other->interface_id == impl->interface_id)
                    return set_error(errbuf, errbuf_len,
                                     "AOT global evidence interface impl is duplicated");
            }
        }
    }

    for (uint32_t i = 0; i < ev->ninterface_impls; i++) {
        const XgInterfaceImplSummary *impl = &ev->interface_impls[i];
        const XgClassSummary *implementor =
            verify_find_evidence_class(ev, impl->implementor_class_id);
        if (!implementor)
            return set_error(errbuf, errbuf_len,
                             "AOT global evidence interface implementor is missing");
        if (!verify_class_interface_range_contains(ev, implementor, impl))
            return set_error(errbuf, errbuf_len,
                             "AOT global evidence interface impl is outside implementor range");
        if (!xaot_bundle_find_interface_use_plan(bundle, impl->interface_id,
                                                 impl->implementor_class_id, XG_NO_ID))
            return set_error(errbuf, errbuf_len, "AOT interface impl has no use plan");
    }

    {
        uint32_t explicit_use_plans = 0;
        for (uint32_t i = 0; i < bundle->ninterface_use_plans; i++) {
            if (bundle->interface_use_plans[i].use_site_id == XG_NO_ID &&
                (bundle->interface_use_plans[i].reason & XAOT_INTERFACE_USE_REASON_IMPLEMENTS) != 0)
                explicit_use_plans++;
        }
        if (explicit_use_plans != ev->ninterface_impls)
            return set_error(errbuf, errbuf_len,
                             "AOT interface-use plan count mismatches evidence");
    }
    return true;
}

static bool verify_interface_extends_reaches(const XgGlobalEvidence *ev, XgInterfaceId from,
                                             XgInterfaceId target, uint32_t depth) {
    if (!ev || from == XG_NO_ID || target == XG_NO_ID || depth > 64)
        return false;
    if (from == target)
        return true;
    for (uint32_t i = 0; i < ev->ninterface_extends; i++) {
        const XgInterfaceExtendsSummary *edge = &ev->interface_extends[i];
        if (edge->child_interface_id != from)
            continue;
        if (verify_interface_extends_reaches(ev, edge->parent_interface_id, target, depth + 1))
            return true;
    }
    return false;
}

static bool verify_interface_impl_matches(const XgGlobalEvidence *ev,
                                          XgInterfaceId implementor_interface,
                                          XgInterfaceId receiver_interface) {
    return implementor_interface == receiver_interface ||
           verify_interface_extends_reaches(ev, implementor_interface, receiver_interface, 0);
}

static bool verify_effective_interface_implementor_seen(const XgGlobalEvidence *ev,
                                                        XgInterfaceId receiver_interface,
                                                        XgClassId implementor_class,
                                                        uint32_t upto_index) {
    if (!ev || receiver_interface == XG_NO_ID || implementor_class == XG_NO_ID)
        return false;
    for (uint32_t i = 0; i < upto_index && i < ev->ninterface_impls; i++) {
        const XgInterfaceImplSummary *impl = &ev->interface_impls[i];
        if (impl->implementor_class_id == implementor_class &&
            verify_interface_impl_matches(ev, impl->interface_id, receiver_interface))
            return true;
    }
    return false;
}

static bool verify_interface_extends_rows(const XgGlobalEvidence *ev, char *errbuf,
                                          size_t errbuf_len) {
    if (!ev)
        return set_error(errbuf, errbuf_len,
                         "AOT global evidence interface extends verifier has no evidence");
    for (uint32_t i = 0; i < ev->ninterface_extends; i++) {
        const XgInterfaceExtendsSummary *edge = &ev->interface_extends[i];
        if (edge->child_interface_id == XG_NO_ID || edge->parent_interface_id == XG_NO_ID ||
            edge->name_id == 0)
            return set_error(errbuf, errbuf_len,
                             "AOT global evidence interface extends identity is stale");
        if (edge->name_id != edge->parent_interface_id)
            return set_error(errbuf, errbuf_len,
                             "AOT global evidence interface extends parent does not re-derive");
        if (!verify_find_evidence_decl_by_kind_name(ev, XG_DECL_INTERFACE,
                                                    edge->child_interface_id))
            return set_error(errbuf, errbuf_len,
                             "AOT global evidence interface extends child is missing");
        if (!verify_find_evidence_decl_by_kind_name(ev, XG_DECL_INTERFACE,
                                                    edge->parent_interface_id))
            return set_error(errbuf, errbuf_len,
                             "AOT global evidence interface extends parent is missing");
        for (uint32_t j = i + 1; j < ev->ninterface_extends; j++) {
            const XgInterfaceExtendsSummary *other = &ev->interface_extends[j];
            if (other->child_interface_id == edge->child_interface_id &&
                other->parent_interface_id == edge->parent_interface_id)
                return set_error(errbuf, errbuf_len,
                                 "AOT global evidence interface extends edge is duplicated");
        }
        if (verify_interface_extends_reaches(ev, edge->parent_interface_id,
                                             edge->child_interface_id, 0))
            return set_error(errbuf, errbuf_len,
                             "AOT global evidence interface extends graph has a cycle");
    }
    return true;
}

static bool verify_interface_method_visible_from(const XgGlobalEvidence *ev,
                                                 XgInterfaceId receiver_interface_id,
                                                 const XgInterfaceMethodSummary *method) {
    if (!ev || receiver_interface_id == XG_NO_ID || !method)
        return false;
    return verify_interface_extends_reaches(ev, receiver_interface_id, method->owner_interface_id,
                                            0);
}

static uint32_t verify_interface_dispatch_slot(const XgGlobalEvidence *ev,
                                               XgInterfaceId receiver_interface_id,
                                               uint32_t name_id, uint32_t signature_key) {
    uint32_t slot = 0;
    if (!ev || receiver_interface_id == XG_NO_ID || name_id == 0 || signature_key == 0)
        return UINT32_MAX;
    for (uint32_t i = 0; i < ev->ninterface_methods; i++) {
        const XgInterfaceMethodSummary *method = &ev->interface_methods[i];
        if (!verify_interface_method_visible_from(ev, receiver_interface_id, method))
            continue;
        if (method->name_id == name_id && method->signature_key == signature_key)
            return slot;
        slot++;
    }
    return UINT32_MAX;
}

static bool verify_interface_method_visibility(const XgGlobalEvidence *ev, char *errbuf,
                                               size_t errbuf_len) {
    if (!ev)
        return set_error(errbuf, errbuf_len,
                         "AOT global evidence interface method verifier has no evidence");
    for (uint32_t di = 0; di < ev->ndecls; di++) {
        const XgDeclSummary *decl = &ev->decls[di];
        if (decl->kind != XG_DECL_INTERFACE)
            continue;
        for (uint32_t i = 0; i < ev->ninterface_methods; i++) {
            const XgInterfaceMethodSummary *left = &ev->interface_methods[i];
            if (!verify_interface_method_visible_from(ev, decl->name_id, left))
                continue;
            for (uint32_t j = i + 1; j < ev->ninterface_methods; j++) {
                const XgInterfaceMethodSummary *right = &ev->interface_methods[j];
                if (!verify_interface_method_visible_from(ev, decl->name_id, right))
                    continue;
                if (left->name_id != right->name_id)
                    continue;
                if (left->signature_key == right->signature_key)
                    return set_error(
                        errbuf, errbuf_len,
                        "AOT global evidence interface method inherited slot is ambiguous");
                return set_error(errbuf, errbuf_len,
                                 "AOT global evidence interface method inherited name conflicts");
            }
        }
    }
    return true;
}

static bool verify_interface_method_rows(const XgGlobalEvidence *ev, char *errbuf,
                                         size_t errbuf_len) {
    if (!ev)
        return set_error(errbuf, errbuf_len,
                         "AOT global evidence interface method verifier has no evidence");

    for (uint32_t i = 0; i < ev->ninterface_methods; i++) {
        const XgInterfaceMethodSummary *method = &ev->interface_methods[i];
        const XgDeclSummary *owner;
        uint32_t owner_count = 0;
        if (method->interface_method_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT global evidence interface method has no id");
        for (uint32_t j = i + 1; j < ev->ninterface_methods; j++) {
            if (ev->interface_methods[j].interface_method_id == method->interface_method_id)
                return set_error(errbuf, errbuf_len,
                                 "AOT global evidence interface method id is duplicated");
        }
        owner = verify_find_evidence_decl_by_kind_name(ev, XG_DECL_INTERFACE,
                                                       method->owner_interface_id);
        if (!owner)
            return set_error(errbuf, errbuf_len,
                             "AOT global evidence interface method owner is missing");
        if (method->name_id == 0 || method->signature_key == 0)
            return set_error(errbuf, errbuf_len,
                             "AOT global evidence interface method identity is stale");
        for (uint32_t j = 0; j < ev->ninterface_methods; j++) {
            const XgInterfaceMethodSummary *candidate = &ev->interface_methods[j];
            if (candidate->owner_interface_id != method->owner_interface_id)
                continue;
            if (candidate != method && candidate->ordinal == method->ordinal)
                return set_error(errbuf, errbuf_len,
                                 "AOT global evidence interface method ordinal is duplicated");
            owner_count++;
        }
        if (owner_count != owner->signature_key)
            return set_error(errbuf, errbuf_len,
                             "AOT global evidence interface method count does not re-derive");
        if (method->ordinal >= owner->signature_key)
            return set_error(errbuf, errbuf_len,
                             "AOT global evidence interface method ordinal is stale");
    }

    for (uint32_t i = 0; i < ev->ndecls; i++) {
        const XgDeclSummary *decl = &ev->decls[i];
        uint32_t method_count = 0;
        if (decl->kind != XG_DECL_INTERFACE)
            continue;
        for (uint32_t j = 0; j < ev->ninterface_methods; j++) {
            if (ev->interface_methods[j].owner_interface_id == decl->name_id)
                method_count++;
        }
        if (method_count != decl->signature_key)
            return set_error(errbuf, errbuf_len,
                             "AOT global evidence interface method count does not re-derive");
    }
    if (!verify_interface_method_visibility(ev, errbuf, errbuf_len))
        return false;
    return true;
}

static uint32_t verify_interface_use_reason_from_object_use(uint32_t reason) {
    uint32_t out = 0;
    if ((reason & XG_INTERFACE_OBJECT_USE_VALUE) != 0)
        out |= XAOT_INTERFACE_USE_REASON_VALUE;
    if ((reason & XG_INTERFACE_OBJECT_USE_ARRAY) != 0)
        out |= XAOT_INTERFACE_USE_REASON_ARRAY;
    if ((reason & XG_INTERFACE_OBJECT_USE_FIELD) != 0)
        out |= XAOT_INTERFACE_USE_REASON_FIELD;
    if ((reason & XG_INTERFACE_OBJECT_USE_RETURN) != 0)
        out |= XAOT_INTERFACE_USE_REASON_RETURN;
    if ((reason & XG_INTERFACE_OBJECT_USE_CAPTURE) != 0)
        out |= XAOT_INTERFACE_USE_REASON_CAPTURE;
    if ((reason & XG_INTERFACE_OBJECT_USE_PARAM) != 0)
        out |= XAOT_INTERFACE_USE_REASON_PARAM;
    return out;
}

static uint32_t verify_interface_object_use_reason_bits(void) {
    return XG_INTERFACE_OBJECT_USE_VALUE | XG_INTERFACE_OBJECT_USE_ARRAY |
           XG_INTERFACE_OBJECT_USE_FIELD | XG_INTERFACE_OBJECT_USE_RETURN |
           XG_INTERFACE_OBJECT_USE_CAPTURE | XG_INTERFACE_OBJECT_USE_PARAM;
}

static bool verify_interface_object_use_rows(const XgGlobalEvidence *ev, char *errbuf,
                                             size_t errbuf_len) {
    if (!ev)
        return set_error(errbuf, errbuf_len,
                         "AOT global evidence interface object use verifier has no evidence");
    for (uint32_t i = 0; i < ev->ninterface_object_uses; i++) {
        const XgInterfaceObjectUseSummary *use = &ev->interface_object_uses[i];
        if (use->use_id == XG_NO_ID || use->interface_id == XG_NO_ID || use->reason == 0 ||
            use->type_key == 0)
            return set_error(errbuf, errbuf_len,
                             "AOT global evidence interface object use identity is stale");
        if (!verify_find_evidence_decl_by_kind_name(ev, XG_DECL_INTERFACE, use->interface_id))
            return set_error(errbuf, errbuf_len,
                             "AOT global evidence interface object use interface is missing");
        if ((use->reason & ~verify_interface_object_use_reason_bits()) != 0)
            return set_error(errbuf, errbuf_len,
                             "AOT global evidence interface object use reason is unknown");
        if (use->owner_func_id != XG_NO_ID &&
            !verify_find_evidence_body_by_func(ev, use->owner_func_id))
            return set_error(errbuf, errbuf_len,
                             "AOT global evidence interface object use owner is missing");
        for (uint32_t j = i + 1; j < ev->ninterface_object_uses; j++) {
            const XgInterfaceObjectUseSummary *other = &ev->interface_object_uses[j];
            if (other->use_id == use->use_id)
                return set_error(errbuf, errbuf_len,
                                 "AOT global evidence interface object use id is duplicated");
        }
    }
    return true;
}

static bool verify_body_summary_ranges(const XgGlobalEvidence *ev, char *errbuf,
                                       size_t errbuf_len) {
    if (!ev)
        return set_error(errbuf, errbuf_len, "AOT global evidence verifier has no evidence");
    for (uint32_t i = 0; i < ev->ncallsites; i++) {
        const XgCallsiteSummary *call = &ev->callsites[i];
        if (call->callsite_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT global evidence callsite has no id");
        switch ((XgCallsiteKind) call->kind) {
            case XG_CALL_DIRECT_FUNC:
                if (call->static_target_func_id == XG_NO_ID)
                    return set_error(errbuf, errbuf_len,
                                     "AOT global evidence direct callsite has no target");
                if (call->receiver_static_class_id != XG_NO_ID ||
                    call->receiver_static_interface_id != XG_NO_ID || call->method_id != XG_NO_ID ||
                    call->method_name_id != 0 || call->method_signature_key != 0)
                    return set_error(errbuf, errbuf_len,
                                     "AOT global evidence direct callsite identity is stale");
                {
                    const XgBodySummary *target_body =
                        verify_find_evidence_body_by_func(ev, call->static_target_func_id);
                    if (!target_body || target_body->kind != XG_BODY_FUNCTION)
                        return set_error(
                            errbuf, errbuf_len,
                            "AOT global evidence direct callsite target body is missing");
                }
                break;
            case XG_CALL_METHOD:
                if (call->static_target_func_id != XG_NO_ID ||
                    call->receiver_static_interface_id != XG_NO_ID ||
                    call->receiver_static_class_id == XG_NO_ID || call->method_id == XG_NO_ID ||
                    call->method_name_id == 0 || call->method_signature_key == 0)
                    return set_error(errbuf, errbuf_len,
                                     "AOT global evidence method callsite identity is stale");
                {
                    const XgMethodSummary *call_method =
                        verify_find_evidence_method_by_id(ev, call->method_id);
                    bool allow_constructor =
                        call_method && (call_method->flags & XG_METHOD_CONSTRUCTOR) != 0;
                    const XgMethodSummary *target_method =
                        verify_find_evidence_method_by_signature_in_hierarchy(
                            ev, call->receiver_static_class_id, call->method_name_id,
                            call->method_signature_key, allow_constructor);
                    if (!target_method || target_method->method_id != call->method_id)
                        return set_error(
                            errbuf, errbuf_len,
                            "AOT global evidence method callsite target does not re-derive");
                }
                break;
            case XG_CALL_INTERFACE:
                if (call->static_target_func_id != XG_NO_ID ||
                    call->receiver_static_class_id != XG_NO_ID ||
                    call->receiver_static_interface_id == XG_NO_ID || call->method_id == XG_NO_ID ||
                    call->method_name_id == 0 || call->method_signature_key == 0)
                    return set_error(errbuf, errbuf_len,
                                     "AOT global evidence interface callsite identity is stale");
                if (!verify_find_evidence_decl_by_kind_name(ev, XG_DECL_INTERFACE,
                                                            call->receiver_static_interface_id))
                    return set_error(
                        errbuf, errbuf_len,
                        "AOT global evidence interface callsite declaration is missing");
                {
                    const XgInterfaceMethodSummary *interface_method =
                        verify_find_evidence_interface_method(
                            ev, call->receiver_static_interface_id, call->method_name_id,
                            call->method_signature_key);
                    if (!interface_method ||
                        interface_method->interface_method_id != call->method_id)
                        return set_error(
                            errbuf, errbuf_len,
                            "AOT global evidence interface callsite method does not re-derive");
                }
                break;
            case XG_CALL_CLOSURE:
                if (call->static_target_func_id != XG_NO_ID ||
                    call->receiver_static_class_id != XG_NO_ID ||
                    call->receiver_static_interface_id != XG_NO_ID || call->method_id != XG_NO_ID ||
                    call->method_name_id != 0 || call->method_signature_key != 0)
                    return set_error(errbuf, errbuf_len,
                                     "AOT global evidence closure callsite identity is stale");
                break;
            case XG_CALL_NATIVE:
                if (call->static_target_func_id != XG_NO_ID ||
                    call->receiver_static_class_id != XG_NO_ID ||
                    call->receiver_static_interface_id != XG_NO_ID || call->method_id == XG_NO_ID ||
                    call->method_name_id == 0 || call->method_signature_key != 0)
                    return set_error(errbuf, errbuf_len,
                                     "AOT global evidence native callsite identity is stale");
                break;
            case XG_CALL_EXTERN:
                if (call->static_target_func_id != XG_NO_ID ||
                    call->receiver_static_class_id != XG_NO_ID ||
                    call->receiver_static_interface_id != XG_NO_ID || call->method_id == XG_NO_ID ||
                    call->method_name_id == 0 || call->method_signature_key != 0)
                    return set_error(errbuf, errbuf_len,
                                     "AOT global evidence extern callsite identity is stale");
                if (!verify_find_evidence_func_decl_by_name_flags(ev, call->method_name_id,
                                                                  XG_DECL_EXTERN))
                    return set_error(errbuf, errbuf_len,
                                     "AOT global evidence extern callsite declaration is missing");
                break;
            default:
                return set_error(errbuf, errbuf_len,
                                 "AOT global evidence callsite kind is invalid");
        }
        for (uint32_t j = i + 1; j < ev->ncallsites; j++) {
            if (ev->callsites[j].callsite_id == call->callsite_id)
                return set_error(errbuf, errbuf_len,
                                 "AOT global evidence callsite id is duplicated");
        }
    }
    for (uint32_t i = 0; i < ev->nbodies; i++) {
        const XgBodySummary *body = &ev->bodies[i];
        const XgDeclSummary *owner_decl = NULL;
        if (body->func_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT global evidence body has no function id");
        for (uint32_t j = i + 1; j < ev->nbodies; j++) {
            if (ev->bodies[j].func_id == body->func_id)
                return set_error(errbuf, errbuf_len,
                                 "AOT global evidence body function id is duplicated");
        }
        if (body->module_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT global evidence body has no module id");
        if (body->name_id == 0)
            return set_error(errbuf, errbuf_len, "AOT global evidence body has no name id");
        switch ((XgBodyKind) body->kind) {
            case XG_BODY_MODULE_INIT:
                if (body->owner_decl_id != XG_NO_ID || body->owner_class_id != XG_NO_ID ||
                    body->owner_method_id != XG_NO_ID || body->signature_key != 0)
                    return set_error(errbuf, errbuf_len,
                                     "AOT global evidence module body has stale owner identity");
                break;
            case XG_BODY_FUNCTION:
                if (body->owner_decl_id == XG_NO_ID || body->owner_class_id != XG_NO_ID ||
                    body->owner_method_id != XG_NO_ID || body->source_span_id == 0)
                    return set_error(errbuf, errbuf_len,
                                     "AOT global evidence function body identity is stale");
                owner_decl = verify_find_evidence_decl(ev, body->owner_decl_id);
                if (!owner_decl)
                    return set_error(errbuf, errbuf_len,
                                     "AOT global evidence function body owner decl is missing");
                if (owner_decl->kind != XG_DECL_FUNC || owner_decl->module_id != body->module_id ||
                    owner_decl->name_id != body->name_id ||
                    owner_decl->signature_key != body->signature_key ||
                    owner_decl->source_span_id != body->source_span_id)
                    return set_error(
                        errbuf, errbuf_len,
                        "AOT global evidence function body owner decl does not re-derive");
                break;
            case XG_BODY_METHOD:
                if (body->owner_decl_id == XG_NO_ID || body->owner_class_id == XG_NO_ID ||
                    body->owner_method_id == XG_NO_ID || body->source_span_id == 0)
                    return set_error(errbuf, errbuf_len,
                                     "AOT global evidence method body identity is stale");
                owner_decl = verify_find_evidence_decl(ev, body->owner_decl_id);
                if (!owner_decl)
                    return set_error(errbuf, errbuf_len,
                                     "AOT global evidence method body owner decl is missing");
                if (owner_decl->kind != XG_DECL_CLASS || owner_decl->module_id != body->module_id)
                    return set_error(
                        errbuf, errbuf_len,
                        "AOT global evidence method body owner decl does not re-derive");
                {
                    const XgClassSummary *owner_class =
                        verify_find_evidence_class(ev, body->owner_class_id);
                    const XgMethodSummary *owner_method =
                        verify_find_evidence_method_by_id(ev, body->owner_method_id);
                    if (!owner_class)
                        return set_error(errbuf, errbuf_len,
                                         "AOT global evidence method body owner class is missing");
                    if (owner_class->decl_id != body->owner_decl_id ||
                        owner_class->module_id != body->module_id)
                        return set_error(
                            errbuf, errbuf_len,
                            "AOT global evidence method body owner class does not re-derive");
                    if (!owner_method)
                        return set_error(errbuf, errbuf_len,
                                         "AOT global evidence method body owner method is missing");
                    if (owner_method->owner_class_id != body->owner_class_id ||
                        owner_method->name_id != body->name_id ||
                        owner_method->signature_key != body->signature_key)
                        return set_error(
                            errbuf, errbuf_len,
                            "AOT global evidence method body owner method does not re-derive");
                }
                break;
            default:
                return set_error(errbuf, errbuf_len, "AOT global evidence body kind is invalid");
        }
        if (body->callsite_count == 0) {
            if (body->callsite_start != 0)
                return set_error(errbuf, errbuf_len,
                                 "AOT global evidence empty body has callsite range");
            continue;
        }
        if (body->callsite_start == XG_NO_ID)
            return set_error(errbuf, errbuf_len,
                             "AOT global evidence body callsite range is missing");
        for (uint32_t j = 0; j < body->callsite_count; j++) {
            const XgCallsiteSummary *call =
                xg_global_evidence_find_callsite(ev, (XgCallsiteId) (body->callsite_start + j));
            if (!call)
                return set_error(errbuf, errbuf_len,
                                 "AOT global evidence body callsite range is stale");
            if (call->owner_func_id != body->func_id)
                return set_error(errbuf, errbuf_len,
                                 "AOT global evidence body callsite owner does not re-derive");
            if (call->body_ordinal != j)
                return set_error(errbuf, errbuf_len,
                                 "AOT global evidence body callsite ordinal does not re-derive");
        }
    }
    for (uint32_t di = 0; di < ev->ndecls; di++) {
        const XgDeclSummary *decl = &ev->decls[di];
        uint32_t body_count = 0;
        bool requires_body;
        if (decl->kind != XG_DECL_FUNC)
            continue;
        for (uint32_t bi = 0; bi < ev->nbodies; bi++) {
            const XgBodySummary *body = &ev->bodies[bi];
            if (body->kind == XG_BODY_FUNCTION && body->owner_decl_id == decl->decl_id)
                body_count++;
        }
        if (body_count > 1)
            return set_error(errbuf, errbuf_len, "AOT global evidence function body is duplicated");
        requires_body = (decl->flags & (XG_DECL_NATIVE | XG_DECL_EXTERN)) == 0;
        if (requires_body && body_count == 0)
            return set_error(errbuf, errbuf_len, "AOT global evidence function decl has no body");
    }
    for (uint32_t mi = 0; mi < ev->nmethods; mi++) {
        const XgMethodSummary *method = &ev->methods[mi];
        uint32_t body_count = 0;
        bool native_method = (method->flags & XG_METHOD_NATIVE) != 0;
        for (uint32_t bi = 0; bi < ev->nbodies; bi++) {
            const XgBodySummary *body = &ev->bodies[bi];
            if (body->kind == XG_BODY_METHOD && body->owner_method_id == method->method_id)
                body_count++;
        }
        if (body_count > 1)
            return set_error(errbuf, errbuf_len, "AOT global evidence method body is duplicated");
        if (native_method && body_count != 0)
            return set_error(errbuf, errbuf_len, "AOT global evidence native method has a body");
        if (!native_method && body_count == 0)
            return set_error(errbuf, errbuf_len, "AOT global evidence method has no body");
    }
    for (uint32_t ci = 0; ci < ev->ncallsites; ci++) {
        const XgCallsiteSummary *call = &ev->callsites[ci];
        uint32_t owning_bodies = 0;
        for (uint32_t bi = 0; bi < ev->nbodies; bi++) {
            const XgBodySummary *body = &ev->bodies[bi];
            uint32_t start = body->callsite_start;
            uint32_t end = start + body->callsite_count;
            if (body->callsite_count == 0 || call->callsite_id < start || call->callsite_id >= end)
                continue;
            owning_bodies++;
            if (call->owner_func_id != body->func_id)
                return set_error(errbuf, errbuf_len,
                                 "AOT global evidence callsite owner body does not re-derive");
            if (call->body_ordinal != call->callsite_id - start)
                return set_error(errbuf, errbuf_len,
                                 "AOT global evidence callsite body ordinal does not re-derive");
        }
        if (owning_bodies == 0)
            return set_error(errbuf, errbuf_len, "AOT global evidence callsite has no body");
        if (owning_bodies > 1)
            return set_error(errbuf, errbuf_len,
                             "AOT global evidence callsite has multiple bodies");
    }
    return true;
}

static bool verify_dispatch_target_anchor_rederives(
    const XgGlobalEvidence *ev, const XaotBundle *bundle, const XaotMethodDispatchPlan *plan,
    uint8_t expected_kind, const XgClassId *expected_classes, const XgMethodId *expected_methods,
    uint16_t expected_count, char *errbuf, size_t errbuf_len) {
    if (!ev || !bundle || !plan)
        return set_error(errbuf, errbuf_len, "AOT dispatch target verifier has incomplete input");

    switch ((XaotMethodDispatchKind) expected_kind) {
        case XAOT_DISPATCH_DIRECT:
            if (expected_count != 1 || plan->target_count != 1 || plan->target_start == 0 ||
                plan->target_start - 1 >= bundle->ndispatch_target_cases)
                return set_error(errbuf, errbuf_len,
                                 "AOT dispatch direct target does not re-derive");
            break;
        case XAOT_DISPATCH_TYPE_SWITCH:
            if (expected_count < 2 || plan->target_count != expected_count ||
                plan->target_start == 0 ||
                plan->target_start - 1 + plan->target_count > bundle->ndispatch_target_cases)
                return set_error(errbuf, errbuf_len,
                                 "AOT dispatch type-switch targets do not re-derive");
            break;
        case XAOT_DISPATCH_VTABLE:
        case XAOT_DISPATCH_ITABLE:
        case XAOT_DISPATCH_RUNTIME_FALLBACK:
            if (plan->target_count != 0 || plan->target_start != 0)
                return set_error(errbuf, errbuf_len,
                                 "AOT dispatch fallback target set does not re-derive");
            return true;
        default:
            return set_error(errbuf, errbuf_len, "AOT dispatch plan has unknown kind");
    }

    for (uint16_t i = 0; i < expected_count; i++) {
        const XaotDispatchTargetCase *target =
            &bundle->dispatch_target_cases[plan->target_start - 1 + i];
        const XgMethodSummary *target_method;
        if (target->callsite_id != plan->callsite_id ||
            target->receiver_class_id != expected_classes[i] ||
            target->method_id != expected_methods[i] || target->evidence != plan->evidence)
            return set_error(errbuf, errbuf_len,
                             expected_kind == XAOT_DISPATCH_TYPE_SWITCH
                                 ? "AOT dispatch type-switch targets do not re-derive"
                                 : "AOT dispatch direct target does not re-derive");
        target_method = verify_find_evidence_method_by_id(ev, target->method_id);
        if (!verify_find_evidence_class(ev, target->receiver_class_id) || !target_method)
            return set_error(errbuf, errbuf_len, "AOT dispatch target is missing");
        if (target->method_owner_class_id != target_method->owner_class_id ||
            target->method_body_func_id !=
                verify_find_method_body_func_id(ev, target_method->method_id) ||
            target->method_name_id != target_method->name_id ||
            target->method_signature_key != target_method->signature_key ||
            target->method_root_id != target_method->root_method_id ||
            target->method_override_depth != target_method->override_depth)
            return set_error(errbuf, errbuf_len,
                             "AOT dispatch target method slot does not re-derive");
    }
    return true;
}

static bool verify_dispatch_vtable_targets_rederive(
    const XgGlobalEvidence *ev, const XaotBundle *bundle, const XaotMethodDispatchPlan *plan,
    const XgCallsiteSummary *call, uint32_t expected_evidence, char *errbuf, size_t errbuf_len) {
    uint16_t expected_count = 0;

    if (!ev || !bundle || !plan || !call)
        return set_error(errbuf, errbuf_len, "AOT dispatch vtable verifier has incomplete input");
    if (call->receiver_static_class_id == XG_NO_ID || call->method_name_id == 0 ||
        call->method_signature_key == 0)
        return set_error(errbuf, errbuf_len, "AOT dispatch vtable callsite is incomplete");
    if (plan->target_start == 0 || plan->target_start > bundle->ndispatch_target_cases)
        return set_error(errbuf, errbuf_len, "AOT dispatch vtable targets do not re-derive");

    for (uint32_t i = 0; i < ev->nclasses; i++) {
        const XgClassSummary *candidate = &ev->classes[i];
        const XgMethodSummary *target_method;
        const XaotDispatchTargetCase *target;
        if (!xg_verify_class_is_runtime_class(candidate))
            continue;
        if (!xg_verify_class_is_descendant_or_self(ev, candidate->class_id,
                                                   call->receiver_static_class_id))
            continue;
        target_method = verify_find_evidence_method_by_signature_in_hierarchy(
            ev, candidate->class_id, call->method_name_id, call->method_signature_key, false);
        if (!target_method)
            return set_error(errbuf, errbuf_len, "AOT dispatch vtable target method is missing");
        if (plan->target_start - 1 + expected_count >= bundle->ndispatch_target_cases)
            return set_error(errbuf, errbuf_len, "AOT dispatch vtable targets do not re-derive");
        target = &bundle->dispatch_target_cases[plan->target_start - 1 + expected_count];
        if (target->callsite_id != plan->callsite_id ||
            target->receiver_class_id != candidate->class_id ||
            target->method_id != target_method->method_id || target->evidence != expected_evidence)
            return set_error(errbuf, errbuf_len, "AOT dispatch vtable targets do not re-derive");
        if (target->method_owner_class_id != target_method->owner_class_id ||
            target->method_body_func_id !=
                verify_find_method_body_func_id(ev, target_method->method_id) ||
            target->method_name_id != target_method->name_id ||
            target->method_signature_key != target_method->signature_key ||
            target->method_root_id != target_method->root_method_id ||
            target->method_override_depth != target_method->override_depth)
            return set_error(errbuf, errbuf_len,
                             "AOT dispatch vtable target method slot does not re-derive");
        expected_count++;
    }

    if (expected_count == 0 || plan->target_count != expected_count ||
        plan->target_start - 1 + plan->target_count > bundle->ndispatch_target_cases)
        return set_error(errbuf, errbuf_len, "AOT dispatch vtable targets do not re-derive");
    return true;
}

static bool verify_dispatch_itable_targets_rederive(
    const XgGlobalEvidence *ev, const XaotBundle *bundle, const XaotMethodDispatchPlan *plan,
    const XgCallsiteSummary *call, uint32_t expected_evidence, char *errbuf, size_t errbuf_len) {
    uint16_t expected_count = 0;

    if (!ev || !bundle || !plan || !call)
        return set_error(errbuf, errbuf_len, "AOT dispatch itable verifier has incomplete input");
    if (call->receiver_static_interface_id == XG_NO_ID || call->method_name_id == 0 ||
        call->method_signature_key == 0)
        return set_error(errbuf, errbuf_len, "AOT dispatch itable callsite is incomplete");
    if (plan->target_start == 0 || plan->target_start > bundle->ndispatch_target_cases)
        return set_error(errbuf, errbuf_len, "AOT dispatch itable targets do not re-derive");

    for (uint32_t i = 0; i < ev->ninterface_impls; i++) {
        const XgInterfaceImplSummary *impl = &ev->interface_impls[i];
        const XgMethodSummary *target_method;
        const XaotDispatchTargetCase *target;
        if (!verify_interface_impl_matches(ev, impl->interface_id,
                                           call->receiver_static_interface_id))
            continue;
        if (verify_effective_interface_implementor_seen(ev, call->receiver_static_interface_id,
                                                        impl->implementor_class_id, i))
            continue;
        target_method = verify_find_evidence_method_by_signature_in_hierarchy(
            ev, impl->implementor_class_id, call->method_name_id, call->method_signature_key,
            false);
        if (!target_method)
            return set_error(errbuf, errbuf_len, "AOT dispatch itable target method is missing");
        if (plan->target_start - 1 + expected_count >= bundle->ndispatch_target_cases)
            return set_error(errbuf, errbuf_len, "AOT dispatch itable targets do not re-derive");
        target = &bundle->dispatch_target_cases[plan->target_start - 1 + expected_count];
        if (target->callsite_id != plan->callsite_id ||
            target->receiver_class_id != impl->implementor_class_id ||
            target->method_id != target_method->method_id || target->evidence != expected_evidence)
            return set_error(errbuf, errbuf_len, "AOT dispatch itable targets do not re-derive");
        if (target->method_owner_class_id != target_method->owner_class_id ||
            target->method_body_func_id !=
                verify_find_method_body_func_id(ev, target_method->method_id) ||
            target->method_name_id != target_method->name_id ||
            target->method_signature_key != target_method->signature_key ||
            target->method_root_id != target_method->root_method_id ||
            target->method_override_depth != target_method->override_depth)
            return set_error(errbuf, errbuf_len,
                             "AOT dispatch itable target method slot does not re-derive");
        expected_count++;
    }

    if (expected_count == 0 || plan->target_count != expected_count ||
        plan->target_start - 1 + plan->target_count > bundle->ndispatch_target_cases)
        return set_error(errbuf, errbuf_len, "AOT dispatch itable targets do not re-derive");
    return true;
}

static bool verify_method_dispatch_plan_rederives(const XgGlobalEvidence *ev,
                                                  const XaotBundle *bundle,
                                                  const XaotMethodDispatchPlan *plan,
                                                  const XgCallsiteSummary *call, char *errbuf,
                                                  size_t errbuf_len) {
    enum {
        XAOT_DISPATCH_SMALL_IMPLEMENTOR_LIMIT = 4
    };
    const XgClassSummary *receiver_cls = NULL;
    const XgMethodSummary *method = NULL;
    uint8_t expected_kind = XAOT_DISPATCH_RUNTIME_FALLBACK;
    uint32_t expected_evidence = XAOT_DISPATCH_EV_GLOBAL_CALLSITE;
    uint8_t expected_reason = XAOT_DISPATCH_UNPROVEN_NONE;
    XgMethodId expected_method_id;
    XgMethodId expected_method_root_id = XG_NO_ID;
    uint32_t expected_dispatch_slot = UINT32_MAX;
    XgClassId expected_classes[XAOT_DISPATCH_SMALL_IMPLEMENTOR_LIMIT];
    XgMethodId expected_methods[XAOT_DISPATCH_SMALL_IMPLEMENTOR_LIMIT];
    uint16_t expected_target_count = 0;

    if (!ev || !bundle || !plan || !call)
        return set_error(errbuf, errbuf_len, "AOT dispatch verifier has incomplete input");

    if (call->kind == XG_CALL_INTERFACE) {
        expected_evidence |= XAOT_DISPATCH_EV_INTERFACE_OBJECT;
        if (call->receiver_static_interface_id == XG_NO_ID) {
            expected_kind = XAOT_DISPATCH_ITABLE;
            expected_reason = XAOT_DISPATCH_UNPROVEN_NO_INTERFACE_ID;
        } else {
            uint32_t implementor_count = 0;
            bool all_targets_resolved = true;
            expected_dispatch_slot =
                verify_interface_dispatch_slot(ev, call->receiver_static_interface_id,
                                               call->method_name_id, call->method_signature_key);
            for (uint32_t i = 0; i < ev->ninterface_impls; i++) {
                const XgInterfaceImplSummary *impl = &ev->interface_impls[i];
                const XgMethodSummary *target_method;
                if (!verify_interface_impl_matches(ev, impl->interface_id,
                                                   call->receiver_static_interface_id))
                    continue;
                if (verify_effective_interface_implementor_seen(
                        ev, call->receiver_static_interface_id, impl->implementor_class_id, i))
                    continue;
                implementor_count++;
                target_method = verify_find_evidence_method_by_signature_in_hierarchy(
                    ev, impl->implementor_class_id, call->method_name_id,
                    call->method_signature_key, false);
                if (!target_method) {
                    all_targets_resolved = false;
                    continue;
                }
                if (implementor_count <= XAOT_DISPATCH_SMALL_IMPLEMENTOR_LIMIT) {
                    expected_classes[implementor_count - 1] = impl->implementor_class_id;
                    expected_methods[implementor_count - 1] = target_method->method_id;
                }
            }
            if (implementor_count == 0 || !all_targets_resolved) {
                expected_kind = XAOT_DISPATCH_ITABLE;
                expected_reason = XAOT_DISPATCH_UNPROVEN_NO_TARGET_METHOD;
            } else if (implementor_count == 1) {
                expected_kind = XAOT_DISPATCH_DIRECT;
                expected_evidence |= XAOT_DISPATCH_EV_SINGLE_IMPLEMENTOR;
                expected_target_count = 1;
            } else if (implementor_count <= XAOT_DISPATCH_SMALL_IMPLEMENTOR_LIMIT) {
                expected_kind = XAOT_DISPATCH_TYPE_SWITCH;
                expected_evidence |= XAOT_DISPATCH_EV_SMALL_IMPLEMENTOR_SET;
                expected_target_count = (uint16_t) implementor_count;
            } else {
                expected_kind = XAOT_DISPATCH_ITABLE;
                expected_reason = XAOT_DISPATCH_UNPROVEN_LARGE_IMPLEMENTOR_SET;
                expected_target_count = (uint16_t) implementor_count;
            }
        }
    } else if (call->method_id == XG_NO_ID) {
        expected_reason = XAOT_DISPATCH_UNPROVEN_NO_METHOD_ID;
    } else if (call->receiver_static_class_id == XG_NO_ID) {
        expected_reason = XAOT_DISPATCH_UNPROVEN_NO_RECEIVER_TYPE;
    } else {
        receiver_cls = verify_find_evidence_class(ev, call->receiver_static_class_id);
        method = verify_find_evidence_method_in_hierarchy(ev, call->receiver_static_class_id,
                                                          call->method_id);
        if (!method) {
            expected_reason = XAOT_DISPATCH_UNPROVEN_NO_METHOD_ID;
        } else if (receiver_cls && (receiver_cls->flags &
                                    (XG_CLASS_EXPLICIT_FINAL | XG_CLASS_INFERRED_FINAL)) != 0) {
            expected_kind = XAOT_DISPATCH_DIRECT;
            expected_evidence |=
                XAOT_DISPATCH_EV_RECEIVER_CONCRETE | XAOT_DISPATCH_EV_INFERRED_FINAL;
            expected_classes[0] = call->receiver_static_class_id;
            expected_methods[0] = method->method_id;
            expected_target_count = 1;
        } else if ((method->flags & XG_METHOD_OVERRIDDEN) == 0) {
            expected_kind = XAOT_DISPATCH_DIRECT;
            expected_evidence |= XAOT_DISPATCH_EV_METHOD_NOT_OVERRIDDEN;
            expected_classes[0] = call->receiver_static_class_id;
            expected_methods[0] = method->method_id;
            expected_target_count = 1;
        } else {
            expected_kind = XAOT_DISPATCH_VTABLE;
            expected_reason = XAOT_DISPATCH_UNPROVEN_POLYMORPHIC;
            expected_evidence |= XAOT_DISPATCH_EV_OVERRIDE_GRAPH;
        }
    }

    expected_method_id = method ? method->method_id : call->method_id;
    expected_method_root_id = method ? method->root_method_id : XG_NO_ID;
    if (expected_kind == XAOT_DISPATCH_RUNTIME_FALLBACK)
        expected_evidence = 0;

    if (plan->callsite_id != call->callsite_id)
        return set_error(errbuf, errbuf_len, "AOT dispatch plan callsite does not re-derive");
    if (plan->owner_func_id != call->owner_func_id)
        return set_error(errbuf, errbuf_len, "AOT dispatch plan owner function does not re-derive");
    if (plan->source_span_id != call->source_span_id)
        return set_error(errbuf, errbuf_len, "AOT dispatch plan source span does not re-derive");
    if (plan->body_ordinal != call->body_ordinal)
        return set_error(errbuf, errbuf_len, "AOT dispatch plan body ordinal does not re-derive");
    if (plan->method_id != expected_method_id)
        return set_error(errbuf, errbuf_len, "AOT dispatch plan method does not re-derive");
    if (plan->method_root_id != expected_method_root_id)
        return set_error(errbuf, errbuf_len, "AOT dispatch plan method root does not re-derive");
    if (plan->method_name_id != call->method_name_id)
        return set_error(errbuf, errbuf_len, "AOT dispatch plan method name does not re-derive");
    if (plan->method_signature_key != call->method_signature_key)
        return set_error(errbuf, errbuf_len,
                         "AOT dispatch plan method signature does not re-derive");
    if (plan->arg_type_key_start != call->arg_type_key_start)
        return set_error(errbuf, errbuf_len,
                         "AOT dispatch plan argument type range does not re-derive");
    if (plan->arg_count != call->arg_count)
        return set_error(errbuf, errbuf_len, "AOT dispatch plan argument count does not re-derive");
    if (plan->receiver_static_class_id != call->receiver_static_class_id)
        return set_error(errbuf, errbuf_len, "AOT dispatch plan receiver class does not re-derive");
    if (plan->receiver_static_interface_id != call->receiver_static_interface_id)
        return set_error(errbuf, errbuf_len,
                         "AOT dispatch plan receiver interface does not re-derive");
    if (plan->kind != expected_kind)
        return set_error(errbuf, errbuf_len, "AOT dispatch plan kind does not re-derive");
    if (plan->dispatch_slot != expected_dispatch_slot)
        return set_error(errbuf, errbuf_len, "AOT dispatch plan slot does not re-derive");
    if (expected_kind == XAOT_DISPATCH_VTABLE) {
        if (!verify_dispatch_vtable_targets_rederive(ev, bundle, plan, call, expected_evidence,
                                                     errbuf, errbuf_len))
            return false;
    } else if (expected_kind == XAOT_DISPATCH_ITABLE &&
               expected_reason == XAOT_DISPATCH_UNPROVEN_LARGE_IMPLEMENTOR_SET) {
        (void) expected_target_count;
        if (!verify_dispatch_itable_targets_rederive(ev, bundle, plan, call, expected_evidence,
                                                     errbuf, errbuf_len))
            return false;
    } else if (!verify_dispatch_target_anchor_rederives(
                   ev, bundle, plan, expected_kind, expected_classes, expected_methods,
                   expected_target_count, errbuf, errbuf_len)) {
        return false;
    }
    if (plan->evidence != expected_evidence)
        return set_error(errbuf, errbuf_len, "AOT dispatch plan evidence does not re-derive");
    if (plan->unproven_reason != expected_reason)
        return set_error(errbuf, errbuf_len, "AOT dispatch plan reason does not re-derive");
    return true;
}

static uint32_t verify_metadata_profile_action(uint32_t profile, uint32_t metadata) {
    if (profile == XG_BUILD_FREESTANDING) {
        switch (metadata) {
            case XG_METADATA_TYPENAME:
            case XG_METADATA_DERIVE:
            case XG_METADATA_DEBUG:
            case XG_METADATA_TOOLING:
                return XAOT_CAPABILITY_ACTION_REJECT;
            default:
                return XAOT_CAPABILITY_ACTION_LINK;
        }
    }
    if (metadata == XG_METADATA_DEBUG || metadata == XG_METADATA_TOOLING)
        return XAOT_CAPABILITY_ACTION_DEBUG_ONLY;
    return XAOT_CAPABILITY_ACTION_LINK;
}

static uint32_t verify_capability_profile_action(uint32_t profile, uint32_t capability) {
    if (capability == XG_CAP_INSTANCEOF)
        return XAOT_CAPABILITY_ACTION_ALLOW;
    if (profile == XG_BUILD_FREESTANDING) {
        switch (capability) {
            case XG_CAP_NATIVE:
            case XG_CAP_EXTERN:
            case XG_CAP_COROUTINE:
            case XG_CAP_CHANNEL:
            case XG_CAP_EXCEPTION:
            case XG_CAP_SYS_THREAD:
            case XG_CAP_SCOPE:
            case XG_CAP_TIMER:
            case XG_CAP_NETPOLL:
            case XG_CAP_TASK:
            case XG_CAP_ATOMIC:
            case XG_CAP_WORK_QUEUE:
            case XG_CAP_RESULT_GROUP:
            case XG_CAP_COUNTDOWN_LATCH:
            case XG_CAP_SEMAPHORE:
            case XG_CAP_EVENT_COUNT:
            case XG_CAP_GENERATOR:
            case XG_CAP_STACKTRACE:
            case XG_CAP_DEEP_COPY:
                return XAOT_CAPABILITY_ACTION_REJECT;
            default:
                return XAOT_CAPABILITY_ACTION_LINK;
        }
    }
    return XAOT_CAPABILITY_ACTION_LINK;
}

static uint32_t verify_capability_transfer_count(const XaotBundle *bundle, uint32_t capability) {
    uint32_t count = 0;

    if (!bundle || capability != XG_CAP_DEEP_COPY)
        return 0;
    for (uint32_t i = 0; i < bundle->ntransfer_plans; i++) {
        if (bundle->transfer_plans[i].action == XAOT_TRANSFER_ACTION_DEEP_COPY)
            count++;
    }
    return count;
}

static bool verify_has_interface_impl(const XgGlobalEvidence *ev, XgInterfaceId interface_id,
                                      XgClassId implementor_class_id) {
    if (!ev || interface_id == XG_NO_ID || implementor_class_id == XG_NO_ID)
        return false;
    for (uint32_t i = 0; i < ev->ninterface_impls; i++) {
        const XgInterfaceImplSummary *impl = &ev->interface_impls[i];
        if (impl->interface_id == interface_id &&
            impl->implementor_class_id == implementor_class_id)
            return true;
    }
    return false;
}

static bool verify_has_effective_interface_impl(const XgGlobalEvidence *ev,
                                                XgInterfaceId interface_id,
                                                XgClassId implementor_class_id) {
    if (!ev || interface_id == XG_NO_ID || implementor_class_id == XG_NO_ID)
        return false;
    for (uint32_t i = 0; i < ev->ninterface_impls; i++) {
        const XgInterfaceImplSummary *impl = &ev->interface_impls[i];
        if (impl->implementor_class_id == implementor_class_id &&
            verify_interface_impl_matches(ev, impl->interface_id, interface_id))
            return true;
    }
    return false;
}

static uint32_t verify_interface_visible_method_count(const XgGlobalEvidence *ev,
                                                      XgInterfaceId interface_id) {
    uint32_t count = 0;
    if (!ev || interface_id == XG_NO_ID)
        return 0;
    for (uint32_t i = 0; i < ev->ninterface_methods; i++) {
        if (verify_interface_method_visible_from(ev, interface_id, &ev->interface_methods[i]))
            count++;
    }
    return count;
}

static uint32_t verify_effective_interface_implementor_count(const XgGlobalEvidence *ev,
                                                             XgInterfaceId interface_id) {
    uint32_t count = 0;
    if (!ev || interface_id == XG_NO_ID)
        return 0;
    for (uint32_t i = 0; i < ev->ninterface_impls; i++) {
        const XgInterfaceImplSummary *impl = &ev->interface_impls[i];
        if (!verify_interface_impl_matches(ev, impl->interface_id, interface_id))
            continue;
        if (verify_effective_interface_implementor_seen(ev, interface_id,
                                                        impl->implementor_class_id, i))
            continue;
        count++;
    }
    return count;
}

static uint32_t verify_interface_object_use_reason_for_interface(const XgGlobalEvidence *ev,
                                                                 XgInterfaceId interface_id) {
    uint32_t reason = 0;
    if (!ev || interface_id == XG_NO_ID)
        return 0;
    for (uint32_t i = 0; i < ev->ninterface_object_uses; i++) {
        const XgInterfaceObjectUseSummary *use = &ev->interface_object_uses[i];
        if (use->interface_id == interface_id)
            reason |= verify_interface_use_reason_from_object_use(use->reason);
    }
    return reason;
}

static bool verify_interface_abi_plan_rederives(const XgGlobalEvidence *ev,
                                                const XaotBundle *bundle,
                                                const XaotInterfaceAbiPlan *plan, char *errbuf,
                                                size_t errbuf_len) {
    uint32_t callsite_count = 0;
    uint32_t object_use_count = 0;
    uint32_t flags = 0;
    uint32_t evidence = 0;
    uint8_t data_source = XAOT_INTERFACE_ABI_SOURCE_NONE;
    uint8_t type_source = XAOT_INTERFACE_ABI_SOURCE_NONE;
    uint8_t itable_source = XAOT_INTERFACE_ABI_SOURCE_NONE;
    uint8_t tag_source = XAOT_INTERFACE_ABI_SOURCE_NONE;

    if (!ev || !bundle || !plan)
        return set_error(errbuf, errbuf_len, "AOT interface ABI verifier has incomplete input");
    if (plan->interface_id == XG_NO_ID)
        return set_error(errbuf, errbuf_len, "AOT interface ABI plan has no interface");

    for (uint32_t i = 0; i < ev->ncallsites; i++) {
        const XgCallsiteSummary *call = &ev->callsites[i];
        const XaotMethodDispatchPlan *dispatch;
        if (call->kind != XG_CALL_INTERFACE ||
            call->receiver_static_interface_id != plan->interface_id)
            continue;
        dispatch = xaot_bundle_find_method_dispatch_plan(bundle, call->callsite_id);
        if (!dispatch)
            return set_error(errbuf, errbuf_len, "AOT interface ABI plan has no dispatch evidence");
        callsite_count++;
        flags |= XAOT_INTERFACE_ABI_NEEDS_IFACE_OBJECT | XAOT_INTERFACE_ABI_BOXED_RECEIVER;
        data_source = XAOT_INTERFACE_ABI_SOURCE_BOXED_VALUE;
        type_source = XAOT_INTERFACE_ABI_SOURCE_NATIVE_TYPE_ID;
        evidence |= XAOT_INTERFACE_ABI_EV_GLOBAL_CALLSITE | XAOT_INTERFACE_ABI_EV_DISPATCH_PLAN;
        if (dispatch->kind == XAOT_DISPATCH_TYPE_SWITCH) {
            flags |= XAOT_INTERFACE_ABI_NEEDS_TYPE_SWITCH_TAG;
            tag_source = XAOT_INTERFACE_ABI_SOURCE_NATIVE_TYPE_ID;
        } else if (dispatch->kind == XAOT_DISPATCH_ITABLE) {
            flags |= XAOT_INTERFACE_ABI_NEEDS_ITABLE;
            itable_source = XAOT_INTERFACE_ABI_SOURCE_DISPATCH_SLOT;
        }
    }
    for (uint32_t i = 0; i < ev->ninterface_object_uses; i++) {
        const XgInterfaceObjectUseSummary *use = &ev->interface_object_uses[i];
        if (use->interface_id != plan->interface_id)
            continue;
        object_use_count++;
        flags |= XAOT_INTERFACE_ABI_NEEDS_IFACE_OBJECT | XAOT_INTERFACE_ABI_BOXED_RECEIVER;
        data_source = XAOT_INTERFACE_ABI_SOURCE_BOXED_VALUE;
        type_source = XAOT_INTERFACE_ABI_SOURCE_NATIVE_TYPE_ID;
        evidence |= XAOT_INTERFACE_ABI_EV_OBJECT_USE;
    }
    if (callsite_count == 0 && object_use_count == 0)
        return set_error(errbuf, errbuf_len, "AOT interface ABI plan has no use evidence");

    if (verify_interface_visible_method_count(ev, plan->interface_id) != 0)
        evidence |= XAOT_INTERFACE_ABI_EV_INTERFACE_METHODS;
    if (verify_effective_interface_implementor_count(ev, plan->interface_id) != 0)
        evidence |= XAOT_INTERFACE_ABI_EV_IMPLEMENTOR_SET;
    if (object_use_count != 0 &&
        verify_interface_visible_method_count(ev, plan->interface_id) != 0) {
        flags |= XAOT_INTERFACE_ABI_NEEDS_ITABLE;
        itable_source = XAOT_INTERFACE_ABI_SOURCE_DISPATCH_SLOT;
    }

    if (plan->callsite_count != callsite_count)
        return set_error(errbuf, errbuf_len, "AOT interface ABI callsite count mismatches");
    if (plan->implementor_count !=
        verify_effective_interface_implementor_count(ev, plan->interface_id))
        return set_error(errbuf, errbuf_len, "AOT interface ABI implementor count mismatches");
    if (plan->method_slot_count != verify_interface_visible_method_count(ev, plan->interface_id))
        return set_error(errbuf, errbuf_len, "AOT interface ABI slot count mismatches");
    if (plan->flags != flags)
        return set_error(errbuf, errbuf_len, "AOT interface ABI flags do not re-derive");
    if (plan->data_source != data_source || plan->type_source != type_source ||
        plan->itable_source != itable_source || plan->tag_source != tag_source)
        return set_error(errbuf, errbuf_len, "AOT interface ABI sources do not re-derive");
    if (plan->evidence != evidence)
        return set_error(errbuf, errbuf_len, "AOT interface ABI evidence does not re-derive");
    if (plan->unproven_reason != XAOT_INTERFACE_ABI_UNPROVEN_NONE)
        return set_error(errbuf, errbuf_len, "AOT interface ABI reason does not re-derive");
    return true;
}

static uint8_t verify_specialization_action_for_dispatch(uint8_t dispatch_kind) {
    switch ((XaotMethodDispatchKind) dispatch_kind) {
        case XAOT_DISPATCH_DIRECT:
            return XAOT_SPECIALIZATION_DIRECT;
        case XAOT_DISPATCH_TYPE_SWITCH:
            return XAOT_SPECIALIZATION_TYPE_SWITCH;
        default:
            return XAOT_SPECIALIZATION_FALLBACK;
    }
}

static uint8_t verify_specialization_reason_for_dispatch(const XaotMethodDispatchPlan *dispatch) {
    if (!dispatch)
        return XAOT_SPECIALIZATION_UNPROVEN_DYNAMIC_BOUNDARY;
    if (dispatch->kind == XAOT_DISPATCH_DIRECT || dispatch->kind == XAOT_DISPATCH_TYPE_SWITCH)
        return XAOT_SPECIALIZATION_UNPROVEN_NONE;
    switch (dispatch->unproven_reason) {
        case XAOT_DISPATCH_UNPROVEN_NO_INTERFACE_ID:
            return XAOT_SPECIALIZATION_UNPROVEN_NO_INTERFACE;
        case XAOT_DISPATCH_UNPROVEN_NO_TARGET_METHOD:
            return XAOT_SPECIALIZATION_UNPROVEN_NO_TARGET;
        case XAOT_DISPATCH_UNPROVEN_LARGE_IMPLEMENTOR_SET:
            return XAOT_SPECIALIZATION_UNPROVEN_LARGE_SET;
        default:
            return XAOT_SPECIALIZATION_UNPROVEN_DYNAMIC_BOUNDARY;
    }
}

static XgClassId verify_specialization_single_implementor(const XaotBundle *bundle,
                                                          const XaotMethodDispatchPlan *dispatch) {
    if (!bundle || !dispatch || dispatch->target_count != 1 || dispatch->target_start == 0)
        return XG_NO_ID;
    if (dispatch->target_start - 1 >= bundle->ndispatch_target_cases)
        return XG_NO_ID;
    return bundle->dispatch_target_cases[dispatch->target_start - 1].receiver_class_id;
}

static bool verify_generic_specialization_plan_rederives(const XgGlobalEvidence *ev,
                                                         const XaotBundle *bundle,
                                                         const XaotGenericSpecializationPlan *plan,
                                                         const XgCallsiteSummary *call,
                                                         char *errbuf, size_t errbuf_len) {
    const XaotMethodDispatchPlan *dispatch;
    uint32_t expected_evidence = XAOT_SPECIALIZATION_EV_GLOBAL_CALLSITE;
    uint32_t implementor_count;
    uint16_t target_count;
    uint8_t dispatch_kind;

    if (!ev || !bundle || !plan || !call)
        return set_error(errbuf, errbuf_len,
                         "AOT generic specialization verifier has incomplete input");
    if (call->kind != XG_CALL_INTERFACE)
        return set_error(errbuf, errbuf_len,
                         "AOT generic specialization plan is not an interface callsite");
    dispatch = xaot_bundle_find_method_dispatch_plan(bundle, call->callsite_id);
    if (!dispatch)
        return set_error(errbuf, errbuf_len,
                         "AOT generic specialization plan has no dispatch plan");
    expected_evidence |= XAOT_SPECIALIZATION_EV_DISPATCH_PLAN;
    implementor_count =
        verify_effective_interface_implementor_count(ev, call->receiver_static_interface_id);
    if (implementor_count != 0)
        expected_evidence |= XAOT_SPECIALIZATION_EV_IMPLEMENTOR_SET;
    target_count = dispatch->target_count;
    if (target_count != 0)
        expected_evidence |= XAOT_SPECIALIZATION_EV_TARGET_CASES;
    dispatch_kind = dispatch->kind;

    if (plan->callsite_id != call->callsite_id || plan->owner_func_id != call->owner_func_id ||
        plan->interface_id != call->receiver_static_interface_id ||
        plan->method_name_id != call->method_name_id ||
        plan->method_signature_key != call->method_signature_key)
        return set_error(errbuf, errbuf_len,
                         "AOT generic specialization identity does not re-derive");
    if (plan->dispatch_kind != dispatch_kind ||
        plan->action != verify_specialization_action_for_dispatch(dispatch_kind) ||
        plan->unproven_reason != verify_specialization_reason_for_dispatch(dispatch))
        return set_error(errbuf, errbuf_len,
                         "AOT generic specialization action does not re-derive");
    if (plan->implementor_count != implementor_count || plan->target_count != target_count)
        return set_error(errbuf, errbuf_len,
                         "AOT generic specialization target set does not re-derive");
    if (plan->single_implementor_class_id !=
        verify_specialization_single_implementor(bundle, dispatch))
        return set_error(errbuf, errbuf_len,
                         "AOT generic specialization single target does not re-derive");
    if (plan->evidence != expected_evidence)
        return set_error(errbuf, errbuf_len,
                         "AOT generic specialization evidence does not re-derive");
    return true;
}

static uint8_t verify_generic_instantiation_action_for(const XgGenericInstSummary *inst) {
    if (!inst)
        return XAOT_GENERIC_INSTANTIATION_RECORD_ROOT;
    if ((inst->flags & XG_GENERIC_INST_SPECIALIZED_BODY) != 0)
        return XAOT_GENERIC_INSTANTIATION_SPECIALIZED_BODY;
    if ((inst->flags & XG_GENERIC_INST_CONCRETE_STORAGE) != 0)
        return XAOT_GENERIC_INSTANTIATION_SPECIALIZED_STORAGE;
    if ((inst->flags & XG_GENERIC_INST_SPECIALIZED_ABI) != 0)
        return XAOT_GENERIC_INSTANTIATION_SPECIALIZED_ABI;
    return XAOT_GENERIC_INSTANTIATION_RECORD_ROOT;
}

static uint8_t verify_generic_instantiation_reason_for(const XgGenericInstSummary *inst) {
    if (!inst || (inst->flags & XG_GENERIC_INST_CONCRETE_TYPES) == 0)
        return XAOT_GENERIC_INST_UNPROVEN_NO_CONCRETE_TYPES;
    if ((inst->flags & XG_GENERIC_INST_SPECIALIZED_BODY) != 0 ||
        (inst->flags & XG_GENERIC_INST_CONCRETE_STORAGE) != 0 ||
        (inst->flags & XG_GENERIC_INST_SPECIALIZED_ABI) != 0)
        return XAOT_GENERIC_INST_UNPROVEN_NONE;
    switch ((XgGenericInstKind) inst->kind) {
        case XG_GENERIC_INST_CLASS:
        case XG_GENERIC_INST_CONTAINER:
            return XAOT_GENERIC_INST_UNPROVEN_NO_SPECIALIZED_STORAGE;
        default:
            return XAOT_GENERIC_INST_UNPROVEN_NO_SPECIALIZED_BODY;
    }
}

static uint32_t verify_generic_instantiation_evidence_for(const XgGenericInstSummary *inst) {
    uint32_t evidence = XAOT_GENERIC_INST_EV_GLOBAL_ROW;
    if (!inst)
        return evidence;
    if ((inst->flags & XG_GENERIC_INST_CONCRETE_TYPES) != 0)
        evidence |= XAOT_GENERIC_INST_EV_CONCRETE_TYPES;
    if (inst->origin_decl_id != XG_NO_ID || inst->origin_func_id != XG_NO_ID ||
        inst->origin_method_id != XG_NO_ID || inst->origin_class_id != XG_NO_ID)
        evidence |= XAOT_GENERIC_INST_EV_ORIGIN_ANCHOR;
    if (inst->root_callsite_id != XG_NO_ID)
        evidence |= XAOT_GENERIC_INST_EV_ROOT_CALLSITE;
    if ((inst->flags & XG_GENERIC_INST_INTERFACE_CONSTRAINT) != 0)
        evidence |= XAOT_GENERIC_INST_EV_INTERFACE_CONSTRAINT;
    if ((inst->flags & XG_GENERIC_INST_SPECIALIZED_BODY) != 0)
        evidence |= XAOT_GENERIC_INST_EV_SPECIALIZED_BODY;
    if ((inst->flags & XG_GENERIC_INST_SPECIALIZED_ABI) != 0)
        evidence |= XAOT_GENERIC_INST_EV_SPECIALIZED_ABI;
    if ((inst->flags & XG_GENERIC_INST_CONCRETE_STORAGE) != 0)
        evidence |= XAOT_GENERIC_INST_EV_SPECIALIZED_STORAGE;
    return evidence;
}

static bool verify_generic_instantiation_plan_rederives(const XaotGenericInstantiationPlan *plan,
                                                        const XgGenericInstSummary *inst,
                                                        char *errbuf, size_t errbuf_len) {
    if (!plan || !inst)
        return set_error(errbuf, errbuf_len,
                         "AOT generic instantiation verifier has incomplete input");
    if (plan->generic_inst_id != inst->generic_inst_id || plan->module_id != inst->module_id ||
        plan->origin_decl_id != inst->origin_decl_id ||
        plan->origin_func_id != inst->origin_func_id ||
        plan->origin_method_id != inst->origin_method_id ||
        plan->origin_class_id != inst->origin_class_id ||
        plan->specialized_func_id != inst->specialized_func_id ||
        plan->specialized_class_id != inst->specialized_class_id ||
        plan->root_callsite_id != inst->root_callsite_id ||
        plan->constraint_interface_id != inst->constraint_interface_id ||
        plan->name_id != inst->name_id || plan->type_key != inst->type_key ||
        plan->type_arg_key_start != inst->type_arg_key_start ||
        plan->type_arg_count != inst->type_arg_count || plan->inst_kind != inst->kind)
        return set_error(errbuf, errbuf_len,
                         "AOT generic instantiation plan identity does not re-derive");
    if (plan->action != verify_generic_instantiation_action_for(inst) ||
        plan->unproven_reason != verify_generic_instantiation_reason_for(inst))
        return set_error(errbuf, errbuf_len, "AOT generic instantiation action does not re-derive");
    if (plan->evidence != verify_generic_instantiation_evidence_for(inst))
        return set_error(errbuf, errbuf_len,
                         "AOT generic instantiation evidence does not re-derive");
    return true;
}

static uint32_t verify_generic_deepen_inst_evidence(const XgGlobalEvidence *ev,
                                                    XgGenericInstId generic_inst_id) {
    const XgGenericInstSummary *inst = xg_global_evidence_find_generic_inst(ev, generic_inst_id);
    if (!inst)
        return 0;
    return XAOT_GENERIC_BODY_EV_GENERIC_INST | XAOT_GENERIC_STORAGE_EV_GENERIC_INST |
           XAOT_GENERIC_CODESIZE_EV_GENERIC_INST;
}

static uint8_t verify_generic_code_size_action_for(const XgGenericCodeSizeSummary *size);

static const XgGenericCodeSizeSummary *
verify_generic_code_size_for_body_use(const XgGlobalEvidence *ev, XgGenericBodyUseId use_id) {
    if (!ev || use_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < ev->ngeneric_code_sizes; i++) {
        const XgGenericCodeSizeSummary *size = &ev->generic_code_sizes[i];
        if (size->body_use_id == use_id)
            return size;
    }
    return NULL;
}

static bool verify_generic_body_uses_code_size_share_policy(const XgGlobalEvidence *ev,
                                                            const XgGenericBodyUseSummary *use) {
    const XgGenericCodeSizeSummary *size =
        use ? verify_generic_code_size_for_body_use(ev, use->use_id) : NULL;
    return size &&
           verify_generic_code_size_action_for(size) == XAOT_GENERIC_CODESIZE_SHARE_CANONICAL_BODY;
}

static uint8_t verify_generic_body_action_for(const XgGlobalEvidence *ev,
                                              const XgGenericBodyUseSummary *use) {
    const XgGenericInstSummary *inst =
        use ? xg_global_evidence_find_generic_inst(ev, use->generic_inst_id) : NULL;
    if (!use || !inst || (inst->flags & XG_GENERIC_INST_CONCRETE_TYPES) == 0)
        return XAOT_GENERIC_BODY_REJECT;
    if ((use->flags & XG_GENERIC_BODY_DYNAMIC_BOUNDARY) != 0)
        return XAOT_GENERIC_BODY_REJECT;
    if (verify_generic_body_uses_code_size_share_policy(ev, use))
        return XAOT_GENERIC_BODY_SHARE_CANONICAL_BODY;
    if (use->specialized_body_func_id != XG_NO_ID ||
        (inst->flags & XG_GENERIC_INST_SPECIALIZED_BODY) != 0)
        return XAOT_GENERIC_BODY_CLONE;
    if ((inst->flags & XG_GENERIC_INST_INTERFACE_CONSTRAINT) != 0)
        return XAOT_GENERIC_BODY_DIRECT_CONSTRAINT_CALL;
    return XAOT_GENERIC_BODY_SHARE_CANONICAL_BODY;
}

static uint8_t verify_generic_body_reason_for(const XgGlobalEvidence *ev,
                                              const XgGenericBodyUseSummary *use) {
    const XgGenericInstSummary *inst =
        use ? xg_global_evidence_find_generic_inst(ev, use->generic_inst_id) : NULL;
    if (!use || !inst || (inst->flags & XG_GENERIC_INST_CONCRETE_TYPES) == 0)
        return XAOT_GENERIC_DEEPEN_UNPROVEN_MISSING_CONCRETE_TYPES;
    if ((use->flags & XG_GENERIC_BODY_DYNAMIC_BOUNDARY) != 0)
        return XAOT_GENERIC_DEEPEN_UNPROVEN_DYNAMIC_BOUNDARY;
    if (verify_generic_body_uses_code_size_share_policy(ev, use))
        return XAOT_GENERIC_DEEPEN_UNPROVEN_CODESIZE_THRESHOLD;
    if (verify_generic_body_action_for(ev, use) == XAOT_GENERIC_BODY_SHARE_CANONICAL_BODY)
        return XAOT_GENERIC_DEEPEN_UNPROVEN_NO_SPECIALIZED_BODY;
    return XAOT_GENERIC_DEEPEN_UNPROVEN_NONE;
}

static uint32_t verify_generic_body_evidence_for(const XgGlobalEvidence *ev,
                                                 const XgGenericBodyUseSummary *use) {
    uint32_t bits = XAOT_GENERIC_BODY_EV_GLOBAL_ROW;
    if (!use)
        return bits;
    bits |= verify_generic_deepen_inst_evidence(ev, use->generic_inst_id) &
            XAOT_GENERIC_BODY_EV_GENERIC_INST;
    if (use->type_key != 0 && use->type_arg_count != 0)
        bits |= XAOT_GENERIC_BODY_EV_TYPE_ARGS;
    if (use->origin_body_func_id != XG_NO_ID)
        bits |= XAOT_GENERIC_BODY_EV_ORIGIN_BODY;
    if (use->specialized_body_func_id != XG_NO_ID)
        bits |= XAOT_GENERIC_BODY_EV_SPECIALIZED_BODY;
    if (use->root_callsite_id != XG_NO_ID)
        bits |= XAOT_GENERIC_BODY_EV_ROOT_CALLSITE;
    return bits;
}

static bool verify_generic_body_plan_rederives(const XgGlobalEvidence *ev,
                                               const XaotGenericBodyPlan *plan,
                                               const XgGenericBodyUseSummary *use, char *errbuf,
                                               size_t errbuf_len) {
    if (!ev || !plan || !use)
        return set_error(errbuf, errbuf_len, "AOT generic body plan verifier has incomplete input");
    if (plan->use_id != use->use_id || plan->generic_inst_id != use->generic_inst_id ||
        plan->module_id != use->module_id || plan->owner_func_id != use->owner_func_id ||
        plan->origin_body_func_id != use->origin_body_func_id ||
        plan->specialized_body_func_id != use->specialized_body_func_id ||
        plan->root_callsite_id != use->root_callsite_id || plan->type_key != use->type_key ||
        plan->type_arg_key_start != use->type_arg_key_start ||
        plan->type_arg_count != use->type_arg_count ||
        plan->estimated_body_size != use->estimated_body_size)
        return set_error(errbuf, errbuf_len, "AOT generic body plan identity does not re-derive");
    if (plan->action != verify_generic_body_action_for(ev, use) ||
        plan->unproven_reason != verify_generic_body_reason_for(ev, use))
        return set_error(errbuf, errbuf_len, "AOT generic body plan action does not re-derive");
    if (plan->evidence != verify_generic_body_evidence_for(ev, use))
        return set_error(errbuf, errbuf_len, "AOT generic body plan evidence does not re-derive");
    return true;
}

static uint8_t verify_generic_storage_action_for(const XgGenericStorageSummary *storage) {
    if (!storage || storage->specialized_type_key == 0)
        return XAOT_GENERIC_STORAGE_REJECT;
    switch ((XgGenericStorageKind) storage->storage_kind) {
        case XG_GENERIC_STORAGE_CLASS:
            return XAOT_GENERIC_STORAGE_SPECIALIZED_CLASS;
        case XG_GENERIC_STORAGE_STRUCT:
            return XAOT_GENERIC_STORAGE_SPECIALIZED_STRUCT;
        case XG_GENERIC_STORAGE_ARRAY:
        case XG_GENERIC_STORAGE_MAP:
        case XG_GENERIC_STORAGE_SET:
            if ((storage->flags & XG_GENERIC_STORAGE_TYPED_INLINE) != 0)
                return XAOT_GENERIC_STORAGE_TYPED_INLINE;
            if ((storage->flags & XG_GENERIC_STORAGE_REF_LANE) != 0)
                return XAOT_GENERIC_STORAGE_REF_LANE;
            if ((storage->flags & XG_GENERIC_STORAGE_BOXED) != 0)
                return XAOT_GENERIC_STORAGE_BOXED;
            return XAOT_GENERIC_STORAGE_REJECT;
        default:
            return XAOT_GENERIC_STORAGE_REJECT;
    }
}

static uint8_t verify_generic_storage_reason_for(const XgGlobalEvidence *ev,
                                                 const XgGenericStorageSummary *storage) {
    const XgGenericInstSummary *inst =
        storage ? xg_global_evidence_find_generic_inst(ev, storage->generic_inst_id) : NULL;
    if (!storage || !inst || (inst->flags & XG_GENERIC_INST_CONCRETE_TYPES) == 0)
        return XAOT_GENERIC_DEEPEN_UNPROVEN_MISSING_CONCRETE_TYPES;
    if (verify_generic_storage_action_for(storage) == XAOT_GENERIC_STORAGE_REJECT)
        return XAOT_GENERIC_DEEPEN_UNPROVEN_UNSUPPORTED_STORAGE;
    return XAOT_GENERIC_DEEPEN_UNPROVEN_NONE;
}

static uint32_t verify_generic_storage_evidence_for(const XgGlobalEvidence *ev,
                                                    const XgGenericStorageSummary *storage) {
    uint32_t bits = XAOT_GENERIC_STORAGE_EV_GLOBAL_ROW;
    if (!storage)
        return bits;
    bits |= verify_generic_deepen_inst_evidence(ev, storage->generic_inst_id) &
            XAOT_GENERIC_STORAGE_EV_GENERIC_INST;
    if (storage->specialized_type_key != 0)
        bits |= XAOT_GENERIC_STORAGE_EV_SPECIALIZED_TYPE;
    if (storage->container_plan_id != 0)
        bits |= XAOT_GENERIC_STORAGE_EV_CONTAINER_PLAN;
    return bits;
}

static bool verify_generic_storage_plan_rederives(const XgGlobalEvidence *ev,
                                                  const XaotGenericStoragePlan *plan,
                                                  const XgGenericStorageSummary *storage,
                                                  char *errbuf, size_t errbuf_len) {
    if (!ev || !plan || !storage)
        return set_error(errbuf, errbuf_len,
                         "AOT generic storage plan verifier has incomplete input");
    if (plan->storage_id != storage->storage_id ||
        plan->generic_inst_id != storage->generic_inst_id ||
        plan->module_id != storage->module_id || plan->storage_kind != storage->storage_kind ||
        plan->origin_type_key != storage->origin_type_key ||
        plan->specialized_type_key != storage->specialized_type_key ||
        plan->elem_type_key != storage->elem_type_key ||
        plan->key_type_key != storage->key_type_key ||
        plan->value_type_key != storage->value_type_key ||
        plan->container_plan_id != storage->container_plan_id)
        return set_error(errbuf, errbuf_len,
                         "AOT generic storage plan identity does not re-derive");
    if (plan->action != verify_generic_storage_action_for(storage) ||
        plan->unproven_reason != verify_generic_storage_reason_for(ev, storage))
        return set_error(errbuf, errbuf_len, "AOT generic storage plan action does not re-derive");
    if (plan->evidence != verify_generic_storage_evidence_for(ev, storage))
        return set_error(errbuf, errbuf_len,
                         "AOT generic storage plan evidence does not re-derive");
    return true;
}

static uint8_t verify_generic_code_size_action_for(const XgGenericCodeSizeSummary *size) {
    if (!size)
        return XAOT_GENERIC_CODESIZE_REJECT;
    if ((size->flags & XG_GENERIC_CODESIZE_FORCE_CLONE) != 0)
        return XAOT_GENERIC_CODESIZE_FORCE_CLONE;
    if ((size->flags & XG_GENERIC_CODESIZE_ALLOW_CLONE) != 0)
        return XAOT_GENERIC_CODESIZE_ALLOW_CLONE;
    if ((size->flags & XG_GENERIC_CODESIZE_SHARE_CANONICAL_BODY) != 0)
        return XAOT_GENERIC_CODESIZE_SHARE_CANONICAL_BODY;
    if (size->threshold != 0 &&
        (uint64_t) size->specialized_body_size_estimate * (uint64_t) size->instantiation_count >
            (uint64_t) size->threshold)
        return XAOT_GENERIC_CODESIZE_SHARE_CANONICAL_BODY;
    return XAOT_GENERIC_CODESIZE_ALLOW_CLONE;
}

static uint8_t verify_generic_code_size_reason_for(const XgGlobalEvidence *ev,
                                                   const XgGenericCodeSizeSummary *size) {
    const XgGenericInstSummary *inst =
        size ? xg_global_evidence_find_generic_inst(ev, size->generic_inst_id) : NULL;
    if (!size || !inst || (inst->flags & XG_GENERIC_INST_CONCRETE_TYPES) == 0)
        return XAOT_GENERIC_DEEPEN_UNPROVEN_MISSING_CONCRETE_TYPES;
    if (verify_generic_code_size_action_for(size) == XAOT_GENERIC_CODESIZE_SHARE_CANONICAL_BODY &&
        (size->flags & XG_GENERIC_CODESIZE_SHARE_CANONICAL_BODY) == 0)
        return XAOT_GENERIC_DEEPEN_UNPROVEN_CODESIZE_THRESHOLD;
    return XAOT_GENERIC_DEEPEN_UNPROVEN_NONE;
}

static uint32_t verify_generic_code_size_evidence_for(const XgGlobalEvidence *ev,
                                                      const XgGenericCodeSizeSummary *size) {
    uint32_t bits = XAOT_GENERIC_CODESIZE_EV_GLOBAL_ROW;
    if (!size)
        return bits;
    bits |= verify_generic_deepen_inst_evidence(ev, size->generic_inst_id) &
            XAOT_GENERIC_CODESIZE_EV_GENERIC_INST;
    if (size->body_use_id != XG_NO_ID)
        bits |= XAOT_GENERIC_CODESIZE_EV_BODY_USE;
    if (size->threshold != 0)
        bits |= XAOT_GENERIC_CODESIZE_EV_THRESHOLD;
    return bits;
}

static bool verify_generic_code_size_plan_rederives(const XgGlobalEvidence *ev,
                                                    const XaotGenericCodeSizePlan *plan,
                                                    const XgGenericCodeSizeSummary *size,
                                                    char *errbuf, size_t errbuf_len) {
    if (!ev || !plan || !size)
        return set_error(errbuf, errbuf_len,
                         "AOT generic code-size plan verifier has incomplete input");
    if (plan->code_size_id != size->code_size_id ||
        plan->generic_inst_id != size->generic_inst_id || plan->module_id != size->module_id ||
        plan->body_use_id != size->body_use_id ||
        plan->origin_body_size_estimate != size->origin_body_size_estimate ||
        plan->specialized_body_size_estimate != size->specialized_body_size_estimate ||
        plan->instantiation_count != size->instantiation_count ||
        plan->threshold != size->threshold)
        return set_error(errbuf, errbuf_len,
                         "AOT generic code-size plan identity does not re-derive");
    if (plan->action != verify_generic_code_size_action_for(size) ||
        plan->unproven_reason != verify_generic_code_size_reason_for(ev, size))
        return set_error(errbuf, errbuf_len,
                         "AOT generic code-size plan action does not re-derive");
    if (plan->evidence != verify_generic_code_size_evidence_for(ev, size))
        return set_error(errbuf, errbuf_len,
                         "AOT generic code-size plan evidence does not re-derive");
    return true;
}

static uint8_t verify_derive_action_for(const XgDeriveSummary *derive) {
    if (!derive)
        return XAOT_DERIVE_REJECT;
    switch ((XgDeriveKind) derive->derive_kind) {
        case XG_DERIVE_JSON:
        case XG_DERIVE_INSPECT:
        case XG_DERIVE_EQ:
        case XG_DERIVE_HASH:
        case XG_DERIVE_CLONE:
            break;
        default:
            return XAOT_DERIVE_REJECT;
    }
    if ((derive->flags & XG_DERIVE_METADATA_ONLY) != 0)
        return XAOT_DERIVE_METADATA_ONLY;
    if ((derive->flags & XG_DERIVE_GENERATED) != 0)
        return XAOT_DERIVE_INLINE_GENERATED_BODY;
    return XAOT_DERIVE_FIELD_TABLE_SIDECAR;
}

static uint8_t verify_derive_reason_for(const XgDeriveSummary *derive) {
    if (!derive)
        return XAOT_DERIVE_UNPROVEN_INVALID_KIND;
    switch ((XgDeriveKind) derive->derive_kind) {
        case XG_DERIVE_JSON:
        case XG_DERIVE_INSPECT:
        case XG_DERIVE_EQ:
        case XG_DERIVE_HASH:
        case XG_DERIVE_CLONE:
            return XAOT_DERIVE_UNPROVEN_NONE;
        default:
            return XAOT_DERIVE_UNPROVEN_INVALID_KIND;
    }
}

static uint32_t verify_derive_evidence_for(const XgDeriveSummary *derive) {
    uint32_t evidence = XAOT_DERIVE_EV_GLOBAL_ROW;
    if (!derive)
        return evidence;
    if ((derive->flags & XG_DERIVE_OPT_IN) != 0)
        evidence |= XAOT_DERIVE_EV_OPT_IN;
    if (derive->field_count != 0)
        evidence |= XAOT_DERIVE_EV_FIELD_TABLE;
    if (derive->method_count != 0)
        evidence |= XAOT_DERIVE_EV_GENERATED_METHOD;
    return evidence;
}

static XgFuncId verify_derive_generated_body_func_id(const XgGlobalEvidence *ev,
                                                     const XgDeriveSummary *derive) {
    if (!ev || !derive || derive->method_count == 0 || derive->method_start == 0)
        return XG_NO_ID;
    if (derive->method_start > ev->nderived_methods)
        return XG_NO_ID;
    return ev->derived_methods[derive->method_start - 1].generated_body_func_id;
}

static bool verify_derive_plan_rederives(const XgGlobalEvidence *ev, const XaotDerivePlan *plan,
                                         const XgDeriveSummary *derive, char *errbuf,
                                         size_t errbuf_len) {
    uint8_t expected_action;
    if (!ev || !plan || !derive)
        return set_error(errbuf, errbuf_len, "AOT derive verifier has incomplete input");
    if (plan->derive_id != derive->derive_id || plan->owner_decl_id != derive->owner_decl_id ||
        plan->type_key != derive->type_key || plan->derive_kind != derive->derive_kind ||
        plan->field_start != derive->field_start || plan->field_count != derive->field_count ||
        plan->method_start != derive->method_start || plan->method_count != derive->method_count)
        return set_error(errbuf, errbuf_len, "AOT derive plan identity does not re-derive");
    expected_action = verify_derive_action_for(derive);
    if (plan->action != expected_action ||
        plan->unproven_reason != verify_derive_reason_for(derive))
        return set_error(errbuf, errbuf_len, "AOT derive plan action does not re-derive");
    if (plan->evidence != verify_derive_evidence_for(derive))
        return set_error(errbuf, errbuf_len, "AOT derive plan evidence does not re-derive");
    if (plan->generated_body_func_id != verify_derive_generated_body_func_id(ev, derive))
        return set_error(errbuf, errbuf_len, "AOT derive plan generated body is stale");
    if (expected_action == XAOT_DERIVE_FIELD_TABLE_SIDECAR && plan->sidecar_index == 0)
        return set_error(errbuf, errbuf_len, "AOT derive sidecar plan has no sidecar index");
    if (expected_action != XAOT_DERIVE_FIELD_TABLE_SIDECAR && plan->sidecar_index != 0)
        return set_error(errbuf, errbuf_len, "AOT derive non-sidecar plan has sidecar index");
    return true;
}

static const XgDeriveSummary *verify_find_derive_for_type_kind(const XgGlobalEvidence *ev,
                                                               uint32_t type_key,
                                                               XgDeclId owner_decl_id,
                                                               uint8_t derive_kind) {
    if (!ev || type_key == 0)
        return NULL;
    for (uint32_t i = 0; i < ev->nderives; i++) {
        const XgDeriveSummary *derive = &ev->derives[i];
        if (derive->type_key == type_key && derive->owner_decl_id == owner_decl_id &&
            derive->derive_kind == derive_kind)
            return derive;
    }
    return NULL;
}

static bool verify_eq_hash_field_range_valid(const XgGlobalEvidence *ev,
                                             const XgDeriveSummary *derive) {
    uint32_t end;
    if (!ev || !derive)
        return false;
    if (derive->field_count == 0)
        return derive->field_start == 0;
    if (derive->field_start == 0)
        return false;
    end = derive->field_start + (uint32_t) derive->field_count - 1;
    return end >= derive->field_start && end <= ev->nderived_fields;
}

static bool verify_eq_hash_fields_match(const XgGlobalEvidence *ev, const XgDeriveSummary *eq,
                                        const XgDeriveSummary *hash) {
    if (!ev || !eq || !hash || eq->field_count != hash->field_count)
        return false;
    if (!verify_eq_hash_field_range_valid(ev, eq) || !verify_eq_hash_field_range_valid(ev, hash))
        return false;
    for (uint32_t i = 0; i < eq->field_count; i++) {
        const XgDerivedFieldSummary *eq_field = &ev->derived_fields[eq->field_start - 1 + i];
        const XgDerivedFieldSummary *hash_field = &ev->derived_fields[hash->field_start - 1 + i];
        if (eq_field->field_ordinal != hash_field->field_ordinal ||
            eq_field->name_id != hash_field->name_id ||
            eq_field->type_key != hash_field->type_key ||
            eq_field->source_field_id != hash_field->source_field_id ||
            eq_field->flags != hash_field->flags)
            return false;
    }
    return true;
}

static uint8_t verify_eq_hash_action_for(const XgGlobalEvidence *ev, const XgDeriveSummary *eq,
                                         const XgDeriveSummary *hash) {
    if (!eq || !hash || eq->type_key != hash->type_key ||
        !verify_eq_hash_fields_match(ev, eq, hash))
        return XAOT_DERIVED_EQ_HASH_REJECT_UNHASHABLE;
    if (eq->method_count != 0 && hash->method_count != 0)
        return XAOT_DERIVED_EQ_HASH_DIRECT_GENERATED_CALL;
    return XAOT_DERIVED_EQ_HASH_BUILTIN_FIELDS_INLINE;
}

static uint8_t verify_eq_hash_reason_for(const XgGlobalEvidence *ev, const XgDeriveSummary *eq,
                                         const XgDeriveSummary *hash) {
    if (!eq)
        return XAOT_EQ_HASH_UNPROVEN_MISSING_EQ;
    if (!hash)
        return XAOT_EQ_HASH_UNPROVEN_MISSING_HASH;
    if (eq->type_key != hash->type_key)
        return XAOT_EQ_HASH_UNPROVEN_TYPE_MISMATCH;
    if (!verify_eq_hash_fields_match(ev, eq, hash))
        return XAOT_EQ_HASH_UNPROVEN_FIELD_MISMATCH;
    return XAOT_EQ_HASH_UNPROVEN_NONE;
}

static uint32_t verify_eq_hash_evidence_for(const XgGlobalEvidence *ev, const XgDeriveSummary *eq,
                                            const XgDeriveSummary *hash) {
    uint32_t evidence = 0;
    if (eq)
        evidence |= XAOT_EQ_HASH_EV_EQ_ROW;
    if (hash)
        evidence |= XAOT_EQ_HASH_EV_HASH_ROW;
    if (eq && hash && eq->type_key == hash->type_key)
        evidence |= XAOT_EQ_HASH_EV_SAME_TYPE;
    if (verify_eq_hash_fields_match(ev, eq, hash))
        evidence |= XAOT_EQ_HASH_EV_SAME_FIELDS;
    if (eq && eq->method_count != 0)
        evidence |= XAOT_EQ_HASH_EV_EQ_BODY;
    if (hash && hash->method_count != 0)
        evidence |= XAOT_EQ_HASH_EV_HASH_BODY;
    return evidence;
}

static bool verify_derived_eq_hash_plan_rederives(const XgGlobalEvidence *ev,
                                                  const XaotDerivedEqHashPlan *plan,
                                                  const XgDeriveSummary *seed, char *errbuf,
                                                  size_t errbuf_len) {
    const XgDeriveSummary *eq;
    const XgDeriveSummary *hash;
    uint32_t expected_field_start;
    uint16_t expected_field_count;
    if (!ev || !plan || !seed)
        return set_error(errbuf, errbuf_len, "AOT Eq/Hash verifier has incomplete input");
    eq = verify_find_derive_for_type_kind(ev, seed->type_key, seed->owner_decl_id, XG_DERIVE_EQ);
    hash =
        verify_find_derive_for_type_kind(ev, seed->type_key, seed->owner_decl_id, XG_DERIVE_HASH);
    expected_field_start = eq ? eq->field_start : (hash ? hash->field_start : 0);
    expected_field_count = eq ? eq->field_count : (hash ? hash->field_count : 0);
    if (plan->owner_decl_id != seed->owner_decl_id || plan->type_key != seed->type_key ||
        plan->eq_derive_id != (eq ? eq->derive_id : XG_NO_ID) ||
        plan->hash_derive_id != (hash ? hash->derive_id : XG_NO_ID) ||
        plan->field_start != expected_field_start || plan->field_count != expected_field_count)
        return set_error(errbuf, errbuf_len,
                         "AOT derived Eq/Hash plan identity does not re-derive");
    if (plan->eq_body_func_id != verify_derive_generated_body_func_id(ev, eq) ||
        plan->hash_body_func_id != verify_derive_generated_body_func_id(ev, hash))
        return set_error(errbuf, errbuf_len, "AOT derived Eq/Hash generated body is stale");
    if (plan->action != verify_eq_hash_action_for(ev, eq, hash) ||
        plan->unproven_reason != verify_eq_hash_reason_for(ev, eq, hash))
        return set_error(errbuf, errbuf_len, "AOT derived Eq/Hash action does not re-derive");
    if (plan->evidence != verify_eq_hash_evidence_for(ev, eq, hash))
        return set_error(errbuf, errbuf_len, "AOT derived Eq/Hash evidence does not re-derive");
    return true;
}

static uint8_t verify_derived_clone_action_for(const XgDeriveSummary *clone) {
    if (!clone || clone->derive_kind != XG_DERIVE_CLONE)
        return XAOT_DERIVED_CLONE_REJECT;
    if (clone->method_count != 0)
        return XAOT_DERIVED_CLONE_DIRECT_GENERATED_CALL;
    if (clone->field_count == 0)
        return XAOT_DERIVED_CLONE_BITWISE_COPY;
    return XAOT_DERIVED_CLONE_FIELDWISE_COPY;
}

static uint8_t verify_derived_clone_reason_for(const XgDeriveSummary *clone) {
    if (!clone || clone->derive_kind != XG_DERIVE_CLONE)
        return XAOT_CLONE_UNPROVEN_MISSING_CLONE;
    return XAOT_CLONE_UNPROVEN_NONE;
}

static uint32_t verify_derived_clone_evidence_for(const XgDeriveSummary *clone) {
    uint32_t evidence = 0;
    if (!clone || clone->derive_kind != XG_DERIVE_CLONE)
        return evidence;
    evidence |= XAOT_CLONE_EV_CLONE_ROW;
    if (clone->field_count != 0)
        evidence |= XAOT_CLONE_EV_FIELD_TABLE;
    if (clone->method_count != 0)
        evidence |= XAOT_CLONE_EV_GENERATED_BODY;
    return evidence;
}

static bool verify_derived_clone_plan_rederives(const XgGlobalEvidence *ev,
                                                const XaotDerivedClonePlan *plan,
                                                const XgDeriveSummary *clone, char *errbuf,
                                                size_t errbuf_len) {
    if (!ev || !plan || !clone)
        return set_error(errbuf, errbuf_len, "AOT derived Clone verifier has incomplete input");
    if (plan->owner_decl_id != clone->owner_decl_id || plan->type_key != clone->type_key ||
        plan->clone_derive_id != clone->derive_id || plan->field_start != clone->field_start ||
        plan->field_count != clone->field_count)
        return set_error(errbuf, errbuf_len, "AOT derived Clone plan identity does not re-derive");
    if (plan->clone_body_func_id != verify_derive_generated_body_func_id(ev, clone))
        return set_error(errbuf, errbuf_len, "AOT derived Clone generated body is stale");
    if (plan->transfer_plan_id != XG_NO_ID)
        return set_error(errbuf, errbuf_len, "AOT derived Clone transfer plan does not re-derive");
    if (plan->action != verify_derived_clone_action_for(clone) ||
        plan->unproven_reason != verify_derived_clone_reason_for(clone))
        return set_error(errbuf, errbuf_len, "AOT derived Clone action does not re-derive");
    if (plan->evidence != verify_derived_clone_evidence_for(clone))
        return set_error(errbuf, errbuf_len, "AOT derived Clone evidence does not re-derive");
    return true;
}

static uint8_t verify_json_shape_action_for(const XgJsonShapeSummary *shape) {
    if (!shape)
        return XAOT_JSON_SHAPE_REJECT;
    switch ((XgJsonShapeKind) shape->shape_kind) {
        case XG_JSON_SHAPE_OPEN:
            return XAOT_JSON_SHAPE_OPEN_DYNAMIC;
        case XG_JSON_SHAPE_SHAPED:
            return XAOT_JSON_SHAPE_HIDDEN_CLASS;
        case XG_JSON_SHAPE_RECORD_BRIDGE:
            return XAOT_JSON_SHAPE_RECORD_BRIDGE;
        default:
            return XAOT_JSON_SHAPE_REJECT;
    }
}

static uint8_t verify_json_shape_reason_for(const XgJsonShapeSummary *shape) {
    return shape && verify_json_shape_kind_valid(shape->shape_kind)
               ? XAOT_JSON_UNPROVEN_NONE
               : XAOT_JSON_UNPROVEN_INVALID_KIND;
}

static uint32_t verify_json_shape_evidence_for(const XgJsonShapeSummary *shape) {
    uint32_t evidence = XAOT_JSON_EV_GLOBAL_ROW;
    if (!shape)
        return evidence;
    if ((shape->flags & XG_JSON_SHAPE_STATIC_KEYS) != 0)
        evidence |= XAOT_JSON_EV_STATIC_KEY;
    if ((shape->flags & XG_JSON_SHAPE_RECORD_BRIDGEABLE) != 0 ||
        shape->shape_kind == XG_JSON_SHAPE_RECORD_BRIDGE)
        evidence |= XAOT_JSON_EV_RECORD_BRIDGE;
    return evidence;
}

static bool verify_json_shape_plan_rederives(const XaotJsonShapePlan *plan,
                                             const XgJsonShapeSummary *shape, char *errbuf,
                                             size_t errbuf_len) {
    if (!plan || !shape)
        return set_error(errbuf, errbuf_len, "AOT Json shape verifier has incomplete input");
    if (plan->json_shape_id != shape->json_shape_id || plan->module_id != shape->module_id ||
        plan->owner_func_id != shape->owner_func_id || plan->type_key != shape->type_key ||
        plan->field_name_start != shape->field_name_start ||
        plan->field_count != shape->field_count || plan->shape_kind != shape->shape_kind ||
        plan->shape_hash != shape->shape_hash)
        return set_error(errbuf, errbuf_len, "AOT Json shape plan identity does not re-derive");
    if (plan->action != verify_json_shape_action_for(shape) ||
        plan->unproven_reason != verify_json_shape_reason_for(shape))
        return set_error(errbuf, errbuf_len, "AOT Json shape plan action does not re-derive");
    if (plan->evidence != verify_json_shape_evidence_for(shape))
        return set_error(errbuf, errbuf_len, "AOT Json shape plan evidence does not re-derive");
    return true;
}

static uint8_t verify_json_access_action_for(const XgGlobalEvidence *ev,
                                             const XgJsonAccessSummary *access) {
    const XgJsonShapeSummary *shape;
    if (!access || !verify_json_access_kind_valid(access->access_kind))
        return XAOT_JSON_ACCESS_REJECT;
    if ((access->flags & XG_JSON_ACCESS_COMPUTED_KEY) != 0)
        return access->receiver_shape_id != XG_NO_ID ? XAOT_JSON_ACCESS_COMPUTED_KEY_GUARD
                                                     : XAOT_JSON_ACCESS_DYNAMIC_LOOKUP;
    if (access->key_name_id == 0)
        return XAOT_JSON_ACCESS_DYNAMIC_LOOKUP;
    if (access->receiver_shape_id == XG_NO_ID)
        return XAOT_JSON_ACCESS_DYNAMIC_LOOKUP;
    shape = xg_global_evidence_find_json_shape(ev, access->receiver_shape_id);
    if (!shape || access->field_ordinal >= shape->field_count)
        return XAOT_JSON_ACCESS_REJECT;
    if (shape->shape_kind == XG_JSON_SHAPE_OPEN)
        return XAOT_JSON_ACCESS_SHAPE_GUARD_INDEX;
    return (access->flags & XG_JSON_ACCESS_RECEIVER_SHAPE_PROVEN) != 0
               ? XAOT_JSON_ACCESS_DIRECT_INDEX
               : XAOT_JSON_ACCESS_SHAPE_GUARD_INDEX;
}

static uint8_t verify_json_access_reason_for(const XgGlobalEvidence *ev,
                                             const XgJsonAccessSummary *access) {
    const XgJsonShapeSummary *shape;
    if (!access || !verify_json_access_kind_valid(access->access_kind))
        return XAOT_JSON_UNPROVEN_INVALID_KIND;
    if ((access->flags & XG_JSON_ACCESS_COMPUTED_KEY) != 0)
        return access->receiver_shape_id != XG_NO_ID ? XAOT_JSON_UNPROVEN_NONE
                                                     : XAOT_JSON_UNPROVEN_COMPUTED_KEY;
    if (access->key_name_id == 0)
        return XAOT_JSON_UNPROVEN_COMPUTED_KEY;
    if (access->receiver_shape_id == XG_NO_ID)
        return XAOT_JSON_UNPROVEN_RECEIVER_SHAPE_UNKNOWN;
    shape = xg_global_evidence_find_json_shape(ev, access->receiver_shape_id);
    if (!shape || access->field_ordinal >= shape->field_count)
        return XAOT_JSON_UNPROVEN_STALE_SHAPE;
    return XAOT_JSON_UNPROVEN_NONE;
}

static uint32_t verify_json_access_evidence_for(const XgGlobalEvidence *ev,
                                                const XgJsonAccessSummary *access) {
    const XgJsonShapeSummary *shape;
    uint32_t evidence = XAOT_JSON_EV_GLOBAL_ROW;
    if (!access)
        return evidence;
    if ((access->flags & XG_JSON_ACCESS_STATIC_KEY) != 0 && access->key_name_id != 0)
        evidence |= XAOT_JSON_EV_STATIC_KEY;
    if (access->receiver_shape_id != XG_NO_ID)
        evidence |= XAOT_JSON_EV_RECEIVER_SHAPE;
    shape = xg_global_evidence_find_json_shape(ev, access->receiver_shape_id);
    if (shape && access->field_ordinal < shape->field_count)
        evidence |= XAOT_JSON_EV_FIELD_INDEX;
    return evidence;
}

static bool verify_json_access_plan_rederives(const XgGlobalEvidence *ev,
                                              const XaotJsonAccessPlan *plan,
                                              const XgJsonAccessSummary *access, char *errbuf,
                                              size_t errbuf_len) {
    if (!ev || !plan || !access)
        return set_error(errbuf, errbuf_len, "AOT Json access verifier has incomplete input");
    if (plan->json_access_id != access->json_access_id || plan->module_id != access->module_id ||
        plan->owner_func_id != access->owner_func_id ||
        plan->receiver_shape_id != access->receiver_shape_id ||
        plan->key_name_id != access->key_name_id ||
        plan->result_type_key != access->result_type_key ||
        plan->field_ordinal != access->field_ordinal || plan->access_kind != access->access_kind)
        return set_error(errbuf, errbuf_len, "AOT Json access plan identity does not re-derive");
    if (plan->action != verify_json_access_action_for(ev, access) ||
        plan->unproven_reason != verify_json_access_reason_for(ev, access))
        return set_error(errbuf, errbuf_len, "AOT Json access plan action does not re-derive");
    if (plan->evidence != verify_json_access_evidence_for(ev, access))
        return set_error(errbuf, errbuf_len, "AOT Json access plan evidence does not re-derive");
    return true;
}

static bool verify_json_codec_kind_valid(uint8_t kind) {
    switch ((XgJsonCodecKind) kind) {
        case XG_JSON_CODEC_PARSE:
        case XG_JSON_CODEC_DECODE:
        case XG_JSON_CODEC_ENCODE:
        case XG_JSON_CODEC_STRINGIFY:
            return true;
        default:
            return false;
    }
}

static uint8_t verify_json_codec_action_for(const XgJsonCodecSummary *codec) {
    if (!codec || !verify_json_codec_kind_valid(codec->codec_kind))
        return XAOT_JSON_CODEC_REJECT;
    switch ((XgJsonCodecKind) codec->codec_kind) {
        case XG_JSON_CODEC_PARSE:
            return (codec->flags & XG_JSON_CODEC_HAS_OUTPUT_SHAPE) != 0
                       ? XAOT_JSON_CODEC_PARSE_RUNTIME_DIRECT
                       : XAOT_JSON_CODEC_PARSE_DOM_BRIDGE;
        case XG_JSON_CODEC_DECODE:
            return (codec->target_type_key != 0 &&
                    (codec->flags & XG_JSON_CODEC_HAS_TARGET_TYPE) != 0)
                       ? XAOT_JSON_CODEC_DECODE_VALIDATE_COPY
                       : XAOT_JSON_CODEC_REJECT;
        case XG_JSON_CODEC_ENCODE:
            return (codec->flags & XG_JSON_CODEC_USES_DERIVE) != 0
                       ? XAOT_JSON_CODEC_ENCODE_DERIVE_SIDECAR
                       : XAOT_JSON_CODEC_ENCODE_FIELD_TABLE;
        case XG_JSON_CODEC_STRINGIFY:
            return XAOT_JSON_CODEC_STRINGIFY_DYNAMIC_WALK;
        default:
            return XAOT_JSON_CODEC_REJECT;
    }
}

static uint8_t verify_json_codec_reason_for(const XgJsonCodecSummary *codec) {
    if (!codec || !verify_json_codec_kind_valid(codec->codec_kind))
        return XAOT_JSON_UNPROVEN_INVALID_KIND;
    if (codec->codec_kind == XG_JSON_CODEC_DECODE &&
        (codec->target_type_key == 0 || (codec->flags & XG_JSON_CODEC_HAS_TARGET_TYPE) == 0))
        return XAOT_JSON_UNPROVEN_MISSING_TARGET_TYPE;
    return XAOT_JSON_UNPROVEN_NONE;
}

static uint32_t verify_json_codec_evidence_for(const XgJsonCodecSummary *codec) {
    uint32_t evidence = XAOT_JSON_EV_GLOBAL_ROW;
    if (!codec)
        return evidence;
    if ((codec->flags & XG_JSON_CODEC_HAS_INPUT_SHAPE) != 0)
        evidence |= XAOT_JSON_EV_INPUT_SHAPE;
    if ((codec->flags & XG_JSON_CODEC_HAS_OUTPUT_SHAPE) != 0)
        evidence |= XAOT_JSON_EV_OUTPUT_SHAPE;
    if ((codec->flags & XG_JSON_CODEC_HAS_TARGET_TYPE) != 0 && codec->target_type_key != 0)
        evidence |= XAOT_JSON_EV_TARGET_TYPE;
    if ((codec->flags & XG_JSON_CODEC_USES_DERIVE) != 0)
        evidence |= XAOT_JSON_EV_DERIVE;
    return evidence;
}

static bool verify_json_codec_plan_rederives(const XaotJsonCodecPlan *plan,
                                             const XgJsonCodecSummary *codec, char *errbuf,
                                             size_t errbuf_len) {
    if (!plan || !codec)
        return set_error(errbuf, errbuf_len, "AOT Json codec verifier has incomplete input");
    if (plan->codec_id != codec->codec_id || plan->module_id != codec->module_id ||
        plan->owner_func_id != codec->owner_func_id ||
        plan->source_span_id != codec->source_span_id || plan->codec_kind != codec->codec_kind ||
        plan->input_type_key != codec->input_type_key ||
        plan->target_type_key != codec->target_type_key ||
        plan->input_shape_id != codec->input_shape_id ||
        plan->output_shape_id != codec->output_shape_id || plan->field_count != codec->field_count)
        return set_error(errbuf, errbuf_len, "AOT Json codec plan identity does not re-derive");
    if (plan->action != verify_json_codec_action_for(codec) ||
        plan->unproven_reason != verify_json_codec_reason_for(codec))
        return set_error(errbuf, errbuf_len, "AOT Json codec plan action does not re-derive");
    if (plan->evidence != verify_json_codec_evidence_for(codec))
        return set_error(errbuf, errbuf_len, "AOT Json codec plan evidence does not re-derive");
    return true;
}

static uint8_t verify_record_shape_action_for(const XgRecordShapeSummary *shape) {
    if (!shape)
        return XAOT_RECORD_SHAPE_REJECT;
    switch ((XgRecordShapeKind) shape->shape_kind) {
        case XG_RECORD_SHAPE_LITERAL:
            return XAOT_RECORD_SHAPE_SEALED_RECORD;
        case XG_RECORD_SHAPE_OPTIONS:
            return XAOT_RECORD_SHAPE_OPTIONS_BAG;
        case XG_RECORD_SHAPE_SPREAD:
            return XAOT_RECORD_SHAPE_SPREAD_RESULT;
        case XG_RECORD_SHAPE_STATIC:
            return XAOT_RECORD_SHAPE_STATIC_RECORD;
        default:
            return XAOT_RECORD_SHAPE_REJECT;
    }
}

static uint8_t verify_record_shape_reason_for(const XgRecordShapeSummary *shape) {
    return shape && verify_record_shape_kind_valid(shape->shape_kind)
               ? XAOT_RECORD_UNPROVEN_NONE
               : XAOT_RECORD_UNPROVEN_INVALID_KIND;
}

static uint32_t verify_record_shape_evidence_for(const XgRecordShapeSummary *shape) {
    uint32_t evidence = XAOT_RECORD_EV_GLOBAL_ROW;
    if (!shape)
        return evidence;
    if ((shape->flags & XG_RECORD_SHAPE_SEALED) != 0)
        evidence |= XAOT_RECORD_EV_SEALED;
    if ((shape->flags & XG_RECORD_SHAPE_STATIC_KEYS) != 0)
        evidence |= XAOT_RECORD_EV_STATIC_FIELD;
    if ((shape->flags & XG_RECORD_SHAPE_JSON_BRIDGEABLE) != 0)
        evidence |= XAOT_RECORD_EV_JSON_BRIDGE;
    return evidence;
}

static bool verify_record_shape_plan_rederives(const XaotRecordShapePlan *plan,
                                               const XgRecordShapeSummary *shape, char *errbuf,
                                               size_t errbuf_len) {
    if (!plan || !shape)
        return set_error(errbuf, errbuf_len, "AOT Record shape verifier has incomplete input");
    if (plan->record_shape_id != shape->record_shape_id || plan->module_id != shape->module_id ||
        plan->owner_func_id != shape->owner_func_id || plan->type_key != shape->type_key ||
        plan->field_name_start != shape->field_name_start ||
        plan->field_count != shape->field_count || plan->shape_kind != shape->shape_kind ||
        plan->shape_hash != shape->shape_hash)
        return set_error(errbuf, errbuf_len, "AOT Record shape plan identity does not re-derive");
    if (plan->action != verify_record_shape_action_for(shape) ||
        plan->unproven_reason != verify_record_shape_reason_for(shape))
        return set_error(errbuf, errbuf_len, "AOT Record shape plan action does not re-derive");
    if (plan->evidence != verify_record_shape_evidence_for(shape))
        return set_error(errbuf, errbuf_len, "AOT Record shape plan evidence does not re-derive");
    return true;
}

static uint8_t verify_record_access_action_for(const XgGlobalEvidence *ev,
                                               const XgRecordAccessSummary *access) {
    const XgRecordShapeSummary *shape;
    if (!access || !verify_record_access_kind_valid(access->access_kind))
        return XAOT_RECORD_ACCESS_REJECT;
    if ((access->flags & XG_RECORD_ACCESS_STATIC_FIELD) == 0 || access->field_name_id == 0)
        return XAOT_RECORD_ACCESS_REJECT;
    if (access->receiver_shape_id == XG_NO_ID)
        return XAOT_RECORD_ACCESS_CHECKED_FIELD;
    shape = xg_global_evidence_find_record_shape(ev, access->receiver_shape_id);
    if (!shape || access->field_ordinal >= shape->field_count)
        return XAOT_RECORD_ACCESS_REJECT;
    if (access->access_kind == XG_RECORD_ACCESS_DESTRUCTURE)
        return XAOT_RECORD_ACCESS_COPY_DESTRUCTURE;
    return (access->flags & XG_RECORD_ACCESS_RECEIVER_SHAPE_PROVEN) != 0
               ? XAOT_RECORD_ACCESS_DIRECT_FIELD
               : XAOT_RECORD_ACCESS_CHECKED_FIELD;
}

static uint8_t verify_record_access_reason_for(const XgGlobalEvidence *ev,
                                               const XgRecordAccessSummary *access) {
    const XgRecordShapeSummary *shape;
    if (!access || !verify_record_access_kind_valid(access->access_kind))
        return XAOT_RECORD_UNPROVEN_INVALID_KIND;
    if ((access->flags & XG_RECORD_ACCESS_STATIC_FIELD) == 0 || access->field_name_id == 0)
        return XAOT_RECORD_UNPROVEN_DYNAMIC_FIELD;
    if (access->receiver_shape_id == XG_NO_ID)
        return XAOT_RECORD_UNPROVEN_RECEIVER_SHAPE_UNKNOWN;
    shape = xg_global_evidence_find_record_shape(ev, access->receiver_shape_id);
    if (!shape || access->field_ordinal >= shape->field_count)
        return XAOT_RECORD_UNPROVEN_STALE_SHAPE;
    return XAOT_RECORD_UNPROVEN_NONE;
}

static uint32_t verify_record_access_evidence_for(const XgGlobalEvidence *ev,
                                                  const XgRecordAccessSummary *access) {
    const XgRecordShapeSummary *shape;
    uint32_t evidence = XAOT_RECORD_EV_GLOBAL_ROW;
    if (!access)
        return evidence;
    if ((access->flags & XG_RECORD_ACCESS_STATIC_FIELD) != 0 && access->field_name_id != 0)
        evidence |= XAOT_RECORD_EV_STATIC_FIELD;
    if (access->receiver_shape_id != XG_NO_ID)
        evidence |= XAOT_RECORD_EV_RECEIVER_SHAPE;
    shape = xg_global_evidence_find_record_shape(ev, access->receiver_shape_id);
    if (shape && access->field_ordinal < shape->field_count)
        evidence |= XAOT_RECORD_EV_FIELD_INDEX;
    return evidence;
}

static bool verify_record_access_plan_rederives(const XgGlobalEvidence *ev,
                                                const XaotRecordAccessPlan *plan,
                                                const XgRecordAccessSummary *access, char *errbuf,
                                                size_t errbuf_len) {
    if (!ev || !plan || !access)
        return set_error(errbuf, errbuf_len, "AOT Record access verifier has incomplete input");
    if (plan->record_access_id != access->record_access_id ||
        plan->module_id != access->module_id || plan->owner_func_id != access->owner_func_id ||
        plan->receiver_shape_id != access->receiver_shape_id ||
        plan->field_name_id != access->field_name_id ||
        plan->result_type_key != access->result_type_key ||
        plan->field_ordinal != access->field_ordinal || plan->access_kind != access->access_kind)
        return set_error(errbuf, errbuf_len, "AOT Record access plan identity does not re-derive");
    if (plan->action != verify_record_access_action_for(ev, access) ||
        plan->unproven_reason != verify_record_access_reason_for(ev, access))
        return set_error(errbuf, errbuf_len, "AOT Record access plan action does not re-derive");
    if (plan->evidence != verify_record_access_evidence_for(ev, access))
        return set_error(errbuf, errbuf_len, "AOT Record access plan evidence does not re-derive");
    return true;
}

static uint8_t verify_map_shape_action_for(const XgMapShapeSummary *shape) {
    if (!shape || !verify_map_container_kind_valid(shape->container_kind) ||
        !verify_map_shape_source_valid(shape->source))
        return XAOT_MAP_SHAPE_REJECT;
    if ((shape->flags & XG_MAP_SHAPE_BOOL_DIRECT) != 0 &&
        !verify_map_shape_supports_bool_direct(shape))
        return XAOT_MAP_SHAPE_REJECT;
    if ((shape->flags & (XG_MAP_SHAPE_STATIC | XG_MAP_SHAPE_READONLY)) ==
        (XG_MAP_SHAPE_STATIC | XG_MAP_SHAPE_READONLY))
        return XAOT_MAP_SHAPE_READONLY_STATIC_TABLE;
    if ((shape->flags & XG_MAP_SHAPE_DENSE_ENUM) != 0)
        return XAOT_MAP_SHAPE_DENSE_ENUM_TABLE;
    if ((shape->flags & XG_MAP_SHAPE_DENSE_INT) != 0)
        return XAOT_MAP_SHAPE_DENSE_INT_TABLE;
    if ((shape->flags & XG_MAP_SHAPE_BOOL_DIRECT) != 0 &&
        verify_map_shape_supports_bool_direct(shape))
        return XAOT_MAP_SHAPE_BOOL_DIRECT;
    if ((shape->flags & XG_MAP_SHAPE_SMALL) != 0)
        return XAOT_MAP_SHAPE_SMALL_INLINE;
    if ((shape->flags & XG_MAP_SHAPE_LITERAL) != 0 || shape->source == XG_MAP_SHAPE_SRC_LITERAL)
        return XAOT_MAP_SHAPE_PREALLOC_HASH;
    return XAOT_MAP_SHAPE_RUNTIME_HASH;
}

static uint8_t verify_map_shape_reason_for(const XgMapShapeSummary *shape) {
    return shape && verify_map_container_kind_valid(shape->container_kind) &&
                   verify_map_shape_source_valid(shape->source)
               ? XAOT_MAP_UNPROVEN_NONE
               : XAOT_MAP_UNPROVEN_INVALID_KIND;
}

static uint32_t verify_map_shape_evidence_for(const XgMapShapeSummary *shape) {
    uint32_t evidence = XAOT_MAP_EV_GLOBAL_ROW;
    if (!shape)
        return evidence;
    if ((shape->flags & XG_MAP_SHAPE_LITERAL) != 0 || shape->source == XG_MAP_SHAPE_SRC_LITERAL)
        evidence |= XAOT_MAP_EV_LITERAL;
    if ((shape->flags & (XG_MAP_SHAPE_DENSE_ENUM | XG_MAP_SHAPE_DENSE_INT)) != 0)
        evidence |= XAOT_MAP_EV_DENSE_DOMAIN;
    if ((shape->flags & XG_MAP_SHAPE_SMALL) != 0)
        evidence |= XAOT_MAP_EV_SMALL;
    if ((shape->flags & XG_MAP_SHAPE_BOOL_DIRECT) != 0 &&
        verify_map_shape_supports_bool_direct(shape))
        evidence |= XAOT_MAP_EV_BOOL_DOMAIN;
    return evidence;
}

static bool verify_map_shape_plan_rederives(const XaotMapShapePlan *plan,
                                            const XgMapShapeSummary *shape, char *errbuf,
                                            size_t errbuf_len) {
    if (!plan || !shape)
        return set_error(errbuf, errbuf_len, "AOT Map/Set shape verifier has incomplete input");
    if (plan->shape_id != shape->shape_id || plan->module_id != shape->module_id ||
        plan->owner_func_id != shape->owner_func_id ||
        plan->container_kind != shape->container_kind || plan->source != shape->source ||
        plan->key_type_key != shape->key_type_key ||
        plan->value_type_key != shape->value_type_key || plan->entry_start != shape->entry_start ||
        plan->entry_count != shape->entry_count || plan->literal_count != shape->literal_count ||
        plan->shape_hash != shape->shape_hash)
        return set_error(errbuf, errbuf_len, "AOT Map/Set shape plan identity does not re-derive");
    if (plan->action != verify_map_shape_action_for(shape) ||
        plan->unproven_reason != verify_map_shape_reason_for(shape))
        return set_error(errbuf, errbuf_len, "AOT Map/Set shape plan action does not re-derive");
    if (plan->evidence != verify_map_shape_evidence_for(shape))
        return set_error(errbuf, errbuf_len, "AOT Map/Set shape plan evidence does not re-derive");
    return true;
}

static uint8_t verify_hash_eq_action_for(const XgHashEqSummary *hash_eq) {
    if (!hash_eq || !verify_hash_eq_kind_valid(hash_eq->kind) || hash_eq->type_key == 0)
        return XAOT_HASH_EQ_DYNAMIC_REJECT;
    switch ((XgHashEqKind) hash_eq->kind) {
        case XG_HASH_EQ_BUILTIN:
        case XG_HASH_EQ_ENUM_ORDINAL:
            return XAOT_HASH_EQ_BUILTIN_INLINE;
        case XG_HASH_EQ_DERIVE:
            return hash_eq->eq_derive_id != XG_NO_ID && hash_eq->hash_derive_id != XG_NO_ID
                       ? XAOT_HASH_EQ_DERIVE_INLINE
                       : XAOT_HASH_EQ_DYNAMIC_REJECT;
        case XG_HASH_EQ_USER_METHOD:
            return hash_eq->eq_func_id != XG_NO_ID && hash_eq->hash_func_id != XG_NO_ID
                       ? XAOT_HASH_EQ_DIRECT_CALL
                       : XAOT_HASH_EQ_DYNAMIC_REJECT;
        case XG_HASH_EQ_MISSING:
        default:
            return XAOT_HASH_EQ_DYNAMIC_REJECT;
    }
}

static uint8_t verify_hash_eq_reason_for(const XgHashEqSummary *hash_eq) {
    if (!hash_eq || !verify_hash_eq_kind_valid(hash_eq->kind) || hash_eq->type_key == 0)
        return XAOT_MAP_UNPROVEN_INVALID_KIND;
    return verify_hash_eq_action_for(hash_eq) == XAOT_HASH_EQ_DYNAMIC_REJECT
               ? XAOT_MAP_UNPROVEN_UNHASHABLE
               : XAOT_MAP_UNPROVEN_NONE;
}

static uint32_t verify_hash_eq_evidence_for(const XgHashEqSummary *hash_eq) {
    uint32_t evidence = XAOT_MAP_EV_GLOBAL_ROW;
    if (hash_eq && verify_hash_eq_action_for(hash_eq) != XAOT_HASH_EQ_DYNAMIC_REJECT)
        evidence |= XAOT_MAP_EV_HASH_EQ;
    return evidence;
}

static bool verify_hash_eq_plan_rederives(const XaotHashEqPlan *plan,
                                          const XgHashEqSummary *hash_eq, char *errbuf,
                                          size_t errbuf_len) {
    if (!plan || !hash_eq)
        return set_error(errbuf, errbuf_len, "AOT Hash/Eq verifier has incomplete input");
    if (plan->hash_eq_id != hash_eq->hash_eq_id || plan->type_key != hash_eq->type_key ||
        plan->kind != hash_eq->kind || plan->eq_derive_id != hash_eq->eq_derive_id ||
        plan->hash_derive_id != hash_eq->hash_derive_id ||
        plan->eq_func_id != hash_eq->eq_func_id || plan->hash_func_id != hash_eq->hash_func_id)
        return set_error(errbuf, errbuf_len, "AOT Hash/Eq plan identity does not re-derive");
    if (plan->action != verify_hash_eq_action_for(hash_eq) ||
        plan->unproven_reason != verify_hash_eq_reason_for(hash_eq))
        return set_error(errbuf, errbuf_len, "AOT Hash/Eq plan action does not re-derive");
    if (plan->evidence != verify_hash_eq_evidence_for(hash_eq))
        return set_error(errbuf, errbuf_len, "AOT Hash/Eq plan evidence does not re-derive");
    return true;
}

static uint8_t verify_key_access_action_for(const XgGlobalEvidence *ev,
                                            const XgKeyAccessSummary *access) {
    const XgMapShapeSummary *shape = NULL;
    const XgHashEqSummary *hash_eq = NULL;
    if (!access || !verify_map_container_kind_valid(access->container_kind) ||
        !verify_key_access_op_valid(access->op))
        return XAOT_KEY_ACCESS_REJECT;
    if (access->receiver_shape_id != XG_NO_ID) {
        shape = xg_global_evidence_find_map_shape(ev, access->receiver_shape_id);
        if (!shape)
            return XAOT_KEY_ACCESS_REJECT;
        bool lookup_op = access->op == XG_KEY_ACCESS_GET || access->op == XG_KEY_ACCESS_INDEX_GET ||
                         access->op == XG_KEY_ACCESS_HAS;
        if (lookup_op && access->container_kind == XG_MAP_CONTAINER_MAP &&
            (shape->flags & XG_MAP_SHAPE_BOOL_DIRECT) != 0 &&
            verify_map_shape_supports_bool_direct(shape))
            return XAOT_KEY_ACCESS_BOOL_DIRECT_LOOKUP;
        if (lookup_op && (shape->flags & (XG_MAP_SHAPE_DENSE_ENUM | XG_MAP_SHAPE_DENSE_INT)) != 0)
            return XAOT_KEY_ACCESS_DIRECT_DENSE_INDEX;
        if (lookup_op && (shape->flags & XG_MAP_SHAPE_SMALL) != 0)
            return XAOT_KEY_ACCESS_INLINE_SMALL_SCAN;
    }
    hash_eq = xg_global_evidence_find_hash_eq(ev, access->key_type_key);
    if (!hash_eq || verify_hash_eq_action_for(hash_eq) == XAOT_HASH_EQ_DYNAMIC_REJECT)
        return XAOT_KEY_ACCESS_GENERIC_HASH_LOOKUP;
    return (access->flags & XG_KEY_ACCESS_CONST_KEY) != 0 && access->key_const_id != 0
               ? XAOT_KEY_ACCESS_PREHASHED_LOOKUP
               : XAOT_KEY_ACCESS_SPECIALIZED_HASH_LOOKUP;
}

static uint8_t verify_key_access_reason_for(const XgGlobalEvidence *ev,
                                            const XgKeyAccessSummary *access) {
    const XgHashEqSummary *hash_eq;
    if (!access || !verify_map_container_kind_valid(access->container_kind) ||
        !verify_key_access_op_valid(access->op))
        return XAOT_MAP_UNPROVEN_INVALID_KIND;
    if (access->receiver_shape_id != XG_NO_ID &&
        !xg_global_evidence_find_map_shape(ev, access->receiver_shape_id))
        return XAOT_MAP_UNPROVEN_MISSING_SHAPE;
    hash_eq = xg_global_evidence_find_hash_eq(ev, access->key_type_key);
    if (!hash_eq)
        return XAOT_MAP_UNPROVEN_MISSING_HASH_EQ;
    if (verify_hash_eq_action_for(hash_eq) == XAOT_HASH_EQ_DYNAMIC_REJECT)
        return XAOT_MAP_UNPROVEN_UNHASHABLE;
    return XAOT_MAP_UNPROVEN_NONE;
}

static uint32_t verify_key_access_evidence_for(const XgGlobalEvidence *ev,
                                               const XgKeyAccessSummary *access) {
    const XgMapShapeSummary *shape;
    const XgHashEqSummary *hash_eq;
    uint32_t bits = XAOT_MAP_EV_GLOBAL_ROW;
    if (!access)
        return bits;
    if ((access->flags & XG_KEY_ACCESS_CONST_KEY) != 0 && access->key_const_id != 0)
        bits |= XAOT_MAP_EV_CONST_KEY;
    if (access->key_prehash != 0)
        bits |= XAOT_MAP_EV_PREHASH;
    shape = xg_global_evidence_find_map_shape(ev, access->receiver_shape_id);
    bool lookup_op = access->op == XG_KEY_ACCESS_GET || access->op == XG_KEY_ACCESS_INDEX_GET ||
                     access->op == XG_KEY_ACCESS_HAS;
    if (lookup_op && shape) {
        if ((shape->flags & (XG_MAP_SHAPE_DENSE_ENUM | XG_MAP_SHAPE_DENSE_INT)) != 0)
            bits |= XAOT_MAP_EV_DENSE_DOMAIN;
        if ((shape->flags & XG_MAP_SHAPE_SMALL) != 0)
            bits |= XAOT_MAP_EV_SMALL;
        if ((shape->flags & XG_MAP_SHAPE_BOOL_DIRECT) != 0 &&
            verify_map_shape_supports_bool_direct(shape))
            bits |= XAOT_MAP_EV_BOOL_DOMAIN;
    }
    hash_eq = xg_global_evidence_find_hash_eq(ev, access->key_type_key);
    if (hash_eq && verify_hash_eq_action_for(hash_eq) != XAOT_HASH_EQ_DYNAMIC_REJECT)
        bits |= XAOT_MAP_EV_HASH_EQ;
    return bits;
}

static bool verify_key_access_plan_rederives(const XgGlobalEvidence *ev,
                                             const XaotKeyAccessPlan *plan,
                                             const XgKeyAccessSummary *access, char *errbuf,
                                             size_t errbuf_len) {
    if (!ev || !plan || !access)
        return set_error(errbuf, errbuf_len, "AOT key access verifier has incomplete input");
    if (plan->access_id != access->access_id || plan->owner_func_id != access->owner_func_id ||
        plan->source_span_id != access->source_span_id ||
        plan->body_ordinal != access->body_ordinal ||
        plan->container_kind != access->container_kind || plan->op != access->op ||
        plan->receiver_shape_id != access->receiver_shape_id ||
        plan->receiver_type_key != access->receiver_type_key ||
        plan->key_type_key != access->key_type_key ||
        plan->value_type_key != access->value_type_key ||
        plan->key_const_id != access->key_const_id || plan->key_prehash != access->key_prehash)
        return set_error(errbuf, errbuf_len, "AOT key access plan identity does not re-derive");
    if (plan->action != verify_key_access_action_for(ev, access) ||
        plan->unproven_reason != verify_key_access_reason_for(ev, access))
        return set_error(errbuf, errbuf_len, "AOT key access plan action does not re-derive");
    if (plan->evidence != verify_key_access_evidence_for(ev, access))
        return set_error(errbuf, errbuf_len, "AOT key access plan evidence does not re-derive");
    return true;
}

static bool verify_sequence_kind_valid(uint8_t kind) {
    switch ((XgSequenceKind) kind) {
        case XG_SEQ_ARRAY:
        case XG_SEQ_BYTES:
        case XG_SEQ_STRING:
        case XG_SEQ_SPAN:
        case XG_SEQ_BYTE_SPAN:
        case XG_SEQ_STRING_BUILDER:
            return true;
        default:
            return false;
    }
}

static bool verify_sequence_access_kind_valid(uint8_t kind) {
    switch ((XgSequenceAccessKind) kind) {
        case XG_SEQ_ACCESS_INDEX_GET:
        case XG_SEQ_ACCESS_INDEX_SET:
        case XG_SEQ_ACCESS_SLICE:
        case XG_SEQ_ACCESS_ITER:
        case XG_SEQ_ACCESS_LENGTH:
            return true;
        default:
            return false;
    }
}

static uint8_t verify_sequence_access_action_for(const XgSequenceAccessSummary *seq) {
    if (!seq || !verify_sequence_kind_valid(seq->sequence_kind) ||
        !verify_sequence_access_kind_valid(seq->access_kind) || seq->receiver_type_key == 0)
        return XAOT_SEQUENCE_ACCESS_REJECT;
    switch ((XgSequenceAccessKind) seq->access_kind) {
        case XG_SEQ_ACCESS_INDEX_GET:
        case XG_SEQ_ACCESS_INDEX_SET:
            return XAOT_SEQUENCE_ACCESS_CHECKED_INDEX;
        case XG_SEQ_ACCESS_SLICE:
            return XAOT_SEQUENCE_ACCESS_CHECKED_SLICE;
        case XG_SEQ_ACCESS_ITER:
            return XAOT_SEQUENCE_ACCESS_ITER_HELPER;
        case XG_SEQ_ACCESS_LENGTH:
            return XAOT_SEQUENCE_ACCESS_DIRECT_LENGTH;
        default:
            return XAOT_SEQUENCE_ACCESS_REJECT;
    }
}

static uint8_t verify_sequence_access_reason_for(const XgSequenceAccessSummary *seq) {
    if (!seq || !verify_sequence_kind_valid(seq->sequence_kind) ||
        !verify_sequence_access_kind_valid(seq->access_kind))
        return XAOT_SEQUENCE_UNPROVEN_INVALID_KIND;
    if (seq->receiver_type_key == 0)
        return XAOT_SEQUENCE_UNPROVEN_MISSING_RECEIVER_TYPE;
    if ((seq->flags & XG_SEQ_ACCESS_NEGATIVE_INDEX) != 0)
        return XAOT_SEQUENCE_UNPROVEN_NEGATIVE_INDEX;
    if ((seq->access_kind == XG_SEQ_ACCESS_INDEX_GET ||
         seq->access_kind == XG_SEQ_ACCESS_INDEX_SET) &&
        (seq->flags & XG_SEQ_ACCESS_CONST_INDEX) == 0)
        return XAOT_SEQUENCE_UNPROVEN_COMPUTED_INDEX;
    if (seq->access_kind == XG_SEQ_ACCESS_SLICE && seq->length_expr_id == 0)
        return XAOT_SEQUENCE_UNPROVEN_DYNAMIC_LENGTH;
    return XAOT_SEQUENCE_UNPROVEN_NONE;
}

static uint32_t verify_sequence_access_evidence_for(const XgSequenceAccessSummary *seq) {
    uint32_t bits = XAOT_SEQUENCE_EV_GLOBAL_ROW;
    if (!seq)
        return bits;
    if (seq->receiver_type_key != 0)
        bits |= XAOT_SEQUENCE_EV_RECEIVER_TYPE;
    if (seq->elem_type_key != 0)
        bits |= XAOT_SEQUENCE_EV_ELEM_TYPE;
    if ((seq->flags & XG_SEQ_ACCESS_CONST_INDEX) != 0)
        bits |= XAOT_SEQUENCE_EV_CONST_INDEX;
    if (seq->length_expr_id != 0)
        bits |= XAOT_SEQUENCE_EV_LENGTH_EXPR;
    if ((seq->flags & XG_SEQ_ACCESS_MUTATING) != 0)
        bits |= XAOT_SEQUENCE_EV_MUTATING;
    return bits;
}

static bool verify_sequence_access_plan_rederives(const XaotSequenceAccessPlan *plan,
                                                  const XgSequenceAccessSummary *seq, char *errbuf,
                                                  size_t errbuf_len) {
    if (!plan || !seq)
        return set_error(errbuf, errbuf_len, "AOT sequence access verifier has incomplete input");
    if (plan->access_id != seq->access_id || plan->owner_func_id != seq->owner_func_id ||
        plan->source_span_id != seq->source_span_id || plan->body_ordinal != seq->body_ordinal ||
        plan->sequence_kind != seq->sequence_kind || plan->access_kind != seq->access_kind ||
        plan->receiver_type_key != seq->receiver_type_key ||
        plan->elem_type_key != seq->elem_type_key || plan->index_expr_id != seq->index_expr_id ||
        plan->length_expr_id != seq->length_expr_id)
        return set_error(errbuf, errbuf_len,
                         "AOT sequence access plan identity does not re-derive");
    if (plan->action != verify_sequence_access_action_for(seq) ||
        plan->unproven_reason != verify_sequence_access_reason_for(seq))
        return set_error(errbuf, errbuf_len, "AOT sequence access plan action does not re-derive");
    if (plan->evidence != verify_sequence_access_evidence_for(seq))
        return set_error(errbuf, errbuf_len,
                         "AOT sequence access plan evidence does not re-derive");
    return true;
}

static bool verify_capacity_op_kind_valid(uint8_t kind) {
    switch ((XgCapacityOpKind) kind) {
        case XG_CAPACITY_PUSH:
        case XG_CAPACITY_APPEND:
        case XG_CAPACITY_EXTEND:
        case XG_CAPACITY_RESERVE:
        case XG_CAPACITY_CONCAT:
        case XG_CAPACITY_TO_STRING:
        case XG_CAPACITY_CLEAR:
            return true;
        default:
            return false;
    }
}

static uint8_t verify_capacity_action_for(const XgCapacityOpSummary *cap) {
    if (!cap || !verify_sequence_kind_valid(cap->sequence_kind) ||
        !verify_capacity_op_kind_valid(cap->op_kind) || cap->receiver_type_key == 0)
        return XAOT_CAPACITY_REJECT;
    switch ((XgCapacityOpKind) cap->op_kind) {
        case XG_CAPACITY_RESERVE:
            return XAOT_CAPACITY_RESERVE_ONCE;
        case XG_CAPACITY_CLEAR:
            return XAOT_CAPACITY_CLEAR_DIRECT;
        case XG_CAPACITY_TO_STRING:
            return XAOT_CAPACITY_BUILDER_FINISH;
        case XG_CAPACITY_PUSH:
        case XG_CAPACITY_APPEND:
        case XG_CAPACITY_EXTEND:
        case XG_CAPACITY_CONCAT:
            return ((cap->flags & (XG_CAPACITY_EXACT_COUNT | XG_CAPACITY_LOOP_APPEND)) != 0)
                       ? XAOT_CAPACITY_RESERVE_ONCE
                       : XAOT_CAPACITY_CHECKED_GROW;
        default:
            return XAOT_CAPACITY_REJECT;
    }
}

static uint8_t verify_capacity_reason_for(const XgCapacityOpSummary *cap) {
    if (!cap || !verify_sequence_kind_valid(cap->sequence_kind) ||
        !verify_capacity_op_kind_valid(cap->op_kind))
        return XAOT_CAPACITY_UNPROVEN_INVALID_KIND;
    if (cap->receiver_type_key == 0)
        return XAOT_CAPACITY_UNPROVEN_MISSING_RECEIVER_TYPE;
    if (verify_capacity_action_for(cap) == XAOT_CAPACITY_CHECKED_GROW)
        return XAOT_CAPACITY_UNPROVEN_COUNT_UNKNOWN;
    return XAOT_CAPACITY_UNPROVEN_NONE;
}

static uint32_t verify_capacity_evidence_for(const XgCapacityOpSummary *cap) {
    uint32_t bits = XAOT_CAPACITY_EV_GLOBAL_ROW;
    if (!cap)
        return bits;
    if (cap->receiver_type_key != 0)
        bits |= XAOT_CAPACITY_EV_RECEIVER_TYPE;
    if (cap->elem_type_key != 0)
        bits |= XAOT_CAPACITY_EV_ELEM_TYPE;
    if ((cap->flags & XG_CAPACITY_EXACT_COUNT) != 0)
        bits |= XAOT_CAPACITY_EV_EXACT_COUNT;
    if ((cap->flags & XG_CAPACITY_LOOP_APPEND) != 0)
        bits |= XAOT_CAPACITY_EV_LOOP_APPEND;
    if ((cap->flags & XG_CAPACITY_MAY_GROW) != 0)
        bits |= XAOT_CAPACITY_EV_MAY_GROW;
    return bits;
}

static bool verify_capacity_plan_rederives(const XaotCapacityPlan *plan,
                                           const XgCapacityOpSummary *cap, char *errbuf,
                                           size_t errbuf_len) {
    if (!plan || !cap)
        return set_error(errbuf, errbuf_len, "AOT capacity verifier has incomplete input");
    if (plan->op_id != cap->op_id || plan->owner_func_id != cap->owner_func_id ||
        plan->source_span_id != cap->source_span_id || plan->body_ordinal != cap->body_ordinal ||
        plan->sequence_kind != cap->sequence_kind || plan->op_kind != cap->op_kind ||
        plan->receiver_type_key != cap->receiver_type_key ||
        plan->elem_type_key != cap->elem_type_key || plan->count_expr_id != cap->count_expr_id ||
        plan->loop_id != cap->loop_id)
        return set_error(errbuf, errbuf_len, "AOT capacity plan identity does not re-derive");
    if (plan->action != verify_capacity_action_for(cap) ||
        plan->unproven_reason != verify_capacity_reason_for(cap))
        return set_error(errbuf, errbuf_len, "AOT capacity plan action does not re-derive");
    if (plan->evidence != verify_capacity_evidence_for(cap))
        return set_error(errbuf, errbuf_len, "AOT capacity plan evidence does not re-derive");
    return true;
}

static bool verify_bulk_op_kind_valid(uint8_t kind) {
    switch ((XgBulkOpKind) kind) {
        case XG_BULK_COPY:
        case XG_BULK_FILL:
        case XG_BULK_COMPARE:
        case XG_BULK_REPEAT:
        case XG_BULK_COPY_WITHIN:
            return true;
        default:
            return false;
    }
}

static uint8_t verify_bulk_action_for(const XgBulkOpSummary *bulk) {
    if (!bulk || !verify_bulk_op_kind_valid(bulk->op_kind))
        return XAOT_BULK_REJECT;
    if (bulk->length_expr_id == 0)
        return XAOT_BULK_RUNTIME_HELPER;
    if ((bulk->flags & XG_BULK_WRITE_BARRIER) != 0)
        return XAOT_BULK_TYPED_LOOP;
    if ((bulk->flags & XG_BULK_POD) == 0)
        return XAOT_BULK_TYPED_LOOP;
    switch ((XgBulkOpKind) bulk->op_kind) {
        case XG_BULK_COPY:
            return (bulk->flags & XG_BULK_OVERLAP_POSSIBLE) != 0 ? XAOT_BULK_INLINE_MEMMOVE
                                                                 : XAOT_BULK_INLINE_MEMCPY;
        case XG_BULK_COPY_WITHIN:
            return XAOT_BULK_INLINE_MEMMOVE;
        case XG_BULK_FILL:
            return XAOT_BULK_INLINE_MEMSET;
        case XG_BULK_COMPARE:
            return XAOT_BULK_INLINE_MEMCMP;
        case XG_BULK_REPEAT:
            return XAOT_BULK_TYPED_LOOP;
        default:
            return XAOT_BULK_REJECT;
    }
}

static uint8_t verify_bulk_reason_for(const XgBulkOpSummary *bulk) {
    if (!bulk || !verify_bulk_op_kind_valid(bulk->op_kind))
        return XAOT_BULK_UNPROVEN_INVALID_KIND;
    if (bulk->length_expr_id == 0)
        return XAOT_BULK_UNPROVEN_LENGTH_UNKNOWN;
    if ((bulk->flags & XG_BULK_WRITE_BARRIER) != 0)
        return XAOT_BULK_UNPROVEN_WRITE_BARRIER;
    if ((bulk->flags & XG_BULK_POD) == 0)
        return XAOT_BULK_UNPROVEN_NON_POD;
    return XAOT_BULK_UNPROVEN_NONE;
}

static uint32_t verify_bulk_evidence_for(const XgBulkOpSummary *bulk) {
    uint32_t bits = XAOT_BULK_EV_GLOBAL_ROW;
    if (!bulk)
        return bits;
    if ((bulk->flags & XG_BULK_POD) != 0)
        bits |= XAOT_BULK_EV_POD;
    if ((bulk->flags & XG_BULK_OVERLAP_POSSIBLE) != 0)
        bits |= XAOT_BULK_EV_OVERLAP_POSSIBLE;
    if ((bulk->flags & XG_BULK_READONLY_SRC) != 0)
        bits |= XAOT_BULK_EV_READONLY_SRC;
    if ((bulk->flags & XG_BULK_WRITE_BARRIER) != 0)
        bits |= XAOT_BULK_EV_WRITE_BARRIER;
    if (bulk->length_expr_id != 0)
        bits |= XAOT_BULK_EV_LENGTH_EXPR;
    return bits;
}

static bool verify_bulk_plan_rederives(const XaotBulkPlan *plan, const XgBulkOpSummary *bulk,
                                       char *errbuf, size_t errbuf_len) {
    if (!plan || !bulk)
        return set_error(errbuf, errbuf_len, "AOT bulk verifier has incomplete input");
    if (plan->op_id != bulk->op_id || plan->owner_func_id != bulk->owner_func_id ||
        plan->source_span_id != bulk->source_span_id || plan->body_ordinal != bulk->body_ordinal ||
        plan->op_kind != bulk->op_kind || plan->elem_type_key != bulk->elem_type_key ||
        plan->src_type_key != bulk->src_type_key || plan->dst_type_key != bulk->dst_type_key ||
        plan->length_expr_id != bulk->length_expr_id)
        return set_error(errbuf, errbuf_len, "AOT bulk plan identity does not re-derive");
    if (plan->action != verify_bulk_action_for(bulk) ||
        plan->unproven_reason != verify_bulk_reason_for(bulk))
        return set_error(errbuf, errbuf_len, "AOT bulk plan action does not re-derive");
    if (plan->evidence != verify_bulk_evidence_for(bulk))
        return set_error(errbuf, errbuf_len, "AOT bulk plan evidence does not re-derive");
    return true;
}

static bool verify_encoding_op_kind_valid(uint8_t kind) {
    switch ((XgEncodingOpKind) kind) {
        case XG_ENCODING_STRING_TO_BYTES:
        case XG_ENCODING_BYTES_TO_STRING:
        case XG_ENCODING_UTF8_VALIDATE:
        case XG_ENCODING_UTF8_COUNT:
        case XG_ENCODING_UTF16_ENCODE:
        case XG_ENCODING_UTF16_DECODE:
            return true;
        default:
            return false;
    }
}

static uint8_t verify_encoding_action_for(const XgEncodingOpSummary *enc) {
    if (!enc || !verify_encoding_op_kind_valid(enc->op_kind))
        return XAOT_ENCODING_REJECT;
    switch ((XgEncodingOpKind) enc->op_kind) {
        case XG_ENCODING_STRING_TO_BYTES:
            return (enc->flags & XG_ENCODING_KNOWN_UTF8) != 0 ? XAOT_ENCODING_VALIDATE_ELIDED
                                                              : XAOT_ENCODING_RUNTIME_VALIDATE;
        case XG_ENCODING_BYTES_TO_STRING:
        case XG_ENCODING_UTF8_COUNT:
            if ((enc->flags & XG_ENCODING_KNOWN_UTF8) != 0)
                return XAOT_ENCODING_VALIDATE_ELIDED;
            if ((enc->flags & XG_ENCODING_VALIDATED_ONCE) != 0)
                return XAOT_ENCODING_VALIDATE_ONCE;
            return XAOT_ENCODING_RUNTIME_VALIDATE;
        case XG_ENCODING_UTF8_VALIDATE:
            return XAOT_ENCODING_RUNTIME_VALIDATE;
        case XG_ENCODING_UTF16_ENCODE:
        case XG_ENCODING_UTF16_DECODE:
            return XAOT_ENCODING_TRANSCODE;
        default:
            return XAOT_ENCODING_REJECT;
    }
}

static uint8_t verify_encoding_reason_for(const XgEncodingOpSummary *enc) {
    if (!enc || !verify_encoding_op_kind_valid(enc->op_kind))
        return XAOT_ENCODING_UNPROVEN_INVALID_KIND;
    if (verify_encoding_action_for(enc) == XAOT_ENCODING_RUNTIME_VALIDATE &&
        (enc->op_kind == XG_ENCODING_BYTES_TO_STRING || enc->op_kind == XG_ENCODING_UTF8_COUNT))
        return XAOT_ENCODING_UNPROVEN_RAW_BYTES_UNKNOWN;
    return XAOT_ENCODING_UNPROVEN_NONE;
}

static uint32_t verify_encoding_evidence_for(const XgEncodingOpSummary *enc) {
    uint32_t bits = XAOT_ENCODING_EV_GLOBAL_ROW;
    if (!enc)
        return bits;
    if ((enc->flags & XG_ENCODING_KNOWN_UTF8) != 0)
        bits |= XAOT_ENCODING_EV_KNOWN_UTF8;
    if ((enc->flags & XG_ENCODING_VALIDATED_ONCE) != 0)
        bits |= XAOT_ENCODING_EV_VALIDATED_ONCE;
    if ((enc->flags & XG_ENCODING_SCALAR_BOUNDARY) != 0)
        bits |= XAOT_ENCODING_EV_SCALAR_BOUNDARY;
    if ((enc->flags & XG_ENCODING_STATIC_LITERAL) != 0)
        bits |= XAOT_ENCODING_EV_STATIC_LITERAL;
    if (enc->input_type_key != 0)
        bits |= XAOT_ENCODING_EV_INPUT_TYPE;
    if (enc->output_type_key != 0)
        bits |= XAOT_ENCODING_EV_OUTPUT_TYPE;
    return bits;
}

static bool verify_encoding_plan_rederives(const XaotEncodingPlan *plan,
                                           const XgEncodingOpSummary *enc, char *errbuf,
                                           size_t errbuf_len) {
    if (!plan || !enc)
        return set_error(errbuf, errbuf_len, "AOT encoding verifier has incomplete input");
    if (plan->op_id != enc->op_id || plan->owner_func_id != enc->owner_func_id ||
        plan->source_span_id != enc->source_span_id || plan->body_ordinal != enc->body_ordinal ||
        plan->op_kind != enc->op_kind || plan->input_type_key != enc->input_type_key ||
        plan->output_type_key != enc->output_type_key)
        return set_error(errbuf, errbuf_len, "AOT encoding plan identity does not re-derive");
    if (plan->action != verify_encoding_action_for(enc) ||
        plan->unproven_reason != verify_encoding_reason_for(enc))
        return set_error(errbuf, errbuf_len, "AOT encoding plan action does not re-derive");
    if (plan->evidence != verify_encoding_evidence_for(enc))
        return set_error(errbuf, errbuf_len, "AOT encoding plan evidence does not re-derive");
    return true;
}

static bool verify_global_evidence_plan(const XaotBundle *bundle, char *errbuf, size_t errbuf_len) {
    const XgGlobalEvidence *ev;
    uint32_t capability_count = 0;
    const uint32_t *capabilities = xg_capability_catalog(&capability_count);
    uint32_t metadata_count = 0;
    const uint32_t *metadata = xg_metadata_catalog(&metadata_count);
    uint32_t static_data_count = 0;
    const uint32_t *static_data = xg_static_data_catalog(&static_data_count);
    uint32_t runtime_class_count = 0;
    uint32_t expected_dispatch_plans = 0;
    uint32_t expected_dispatch_target_cases = 0;
    uint32_t expected_interface_abi_plans = 0;
    uint32_t expected_generic_specialization_plans = 0;
    uint32_t expected_generic_instantiation_plans = 0;
    uint32_t expected_generic_body_plans = 0;
    uint32_t expected_generic_storage_plans = 0;
    uint32_t expected_generic_code_size_plans = 0;
    uint32_t expected_derive_plans = 0;
    uint32_t expected_derived_eq_hash_plans = 0;
    uint32_t expected_derived_clone_plans = 0;
    uint32_t expected_json_shape_plans = 0;
    uint32_t expected_json_access_plans = 0;
    uint32_t expected_json_codec_plans = 0;
    uint32_t expected_record_shape_plans = 0;
    uint32_t expected_record_access_plans = 0;
    uint32_t expected_map_shape_plans = 0;
    uint32_t expected_key_access_plans = 0;
    uint32_t expected_hash_eq_plans = 0;
    uint32_t expected_sequence_access_plans = 0;
    uint32_t expected_capacity_plans = 0;
    uint32_t expected_bulk_plans = 0;
    uint32_t expected_encoding_plans = 0;
    uint32_t expected_metadata_plans = 0;
    uint32_t expected_capability_plans = 0;
    uint32_t expected_static_data_plans = 0;
    uint32_t expected_link_dependency_plans = 0;

    if (!bundle)
        return set_error(errbuf, errbuf_len, "AOT global evidence verifier has no bundle");
    ev = bundle->global_evidence_plan.evidence;
    if (!ev)
        return set_error(errbuf, errbuf_len, "AOT bundle has no global evidence plan");
    if (bundle->global_evidence_plan.evidence_hash != xg_global_evidence_hash(ev))
        return set_error(errbuf, errbuf_len, "AOT global evidence hash is stale");

    if (!verify_interface_extends_rows(ev, errbuf, errbuf_len))
        return false;
    if (!verify_interface_method_rows(ev, errbuf, errbuf_len))
        return false;
    if (!verify_interface_object_use_rows(ev, errbuf, errbuf_len))
        return false;
    if (!verify_body_summary_ranges(ev, errbuf, errbuf_len))
        return false;
    if (!verify_generic_inst_rows(ev, errbuf, errbuf_len))
        return false;
    if (!verify_generic_deepen_rows(ev, errbuf, errbuf_len))
        return false;
    if (!verify_derive_rows(ev, errbuf, errbuf_len))
        return false;
    if (!verify_json_rows(ev, errbuf, errbuf_len))
        return false;
    if (!verify_record_rows(ev, errbuf, errbuf_len))
        return false;
    if (!verify_map_rows(ev, errbuf, errbuf_len))
        return false;

    for (uint32_t di = 0; di < ev->ndecls; di++) {
        const XgDeclSummary *decl = &ev->decls[di];
        bool has_derive_flag = (decl->flags & XG_DECL_DERIVE) != 0;
        bool has_derive_bits = decl->derive_flags != 0;
        if (has_derive_flag != has_derive_bits)
            return set_error(errbuf, errbuf_len,
                             "AOT global evidence derive flags do not re-derive");
    }

    for (uint32_t i = 0; i < ev->nclasses; i++) {
        const XgClassSummary *cls = &ev->classes[i];
        const XaotClassHierarchyPlan *hier;
        const XaotClassLayoutPlan *layout;
        bool actual_has_subclass;
        bool flag_has_subclass;
        bool expected_inferred_final;
        if (!xg_verify_class_is_runtime_class(cls))
            continue;
        runtime_class_count++;
        if (cls->parent_class_id != XG_NO_ID &&
            !verify_find_evidence_class(ev, cls->parent_class_id))
            return set_error(errbuf, errbuf_len, "AOT global evidence class parent is missing");
        if ((cls->flags & XG_CLASS_GENERIC_SKELETON) != 0 &&
            (cls->flags & XG_CLASS_MONOMORPHIZED) != 0)
            return set_error(errbuf, errbuf_len,
                             "AOT generic class evidence is both skeleton and monomorphized");
        if ((cls->flags & XG_CLASS_MONOMORPHIZED) != 0) {
            if (cls->generic_origin_class_id == XG_NO_ID ||
                !verify_find_evidence_class(ev, cls->generic_origin_class_id))
                return set_error(errbuf, errbuf_len, "AOT monomorphized class origin is missing");
            if (cls->generic_origin_name_id == 0 || cls->generic_type_key == 0 ||
                cls->generic_type_arg_key_start == 0 || cls->generic_type_arg_count == 0)
                return set_error(errbuf, errbuf_len,
                                 "AOT monomorphized class generic identity is incomplete");
            if (!verify_monomorphized_class_has_generic_inst_anchor(ev, cls))
                return set_error(errbuf, errbuf_len,
                                 "AOT monomorphized class generic inst anchor is missing");
        }
        actual_has_subclass = xg_verify_class_has_subclass(ev, cls->class_id);
        flag_has_subclass = (cls->flags & XG_CLASS_HAS_SUBCLASS) != 0;
        expected_inferred_final = !actual_has_subclass;
        if (flag_has_subclass != actual_has_subclass)
            return set_error(errbuf, errbuf_len,
                             "AOT global evidence has_subclass flag does not re-derive");
        if (((cls->flags & XG_CLASS_INFERRED_FINAL) != 0) != expected_inferred_final)
            return set_error(errbuf, errbuf_len,
                             "AOT global evidence inferred-final flag does not re-derive");
        if ((cls->flags & XG_CLASS_EXPLICIT_FINAL) != 0 && actual_has_subclass)
            return set_error(errbuf, errbuf_len, "AOT global evidence final class has subclass");
        hier = xaot_bundle_find_class_hierarchy_plan(bundle, cls->class_id);
        if (!hier)
            return set_error(errbuf, errbuf_len, "AOT class has no hierarchy plan");
        if (hier->parent_class_id != cls->parent_class_id || hier->flags != cls->flags)
            return set_error(errbuf, errbuf_len, "AOT class hierarchy plan mismatches evidence");
        if (hier->evidence == 0 || hier->unproven_reason != XAOT_CLASS_UNPROVEN_NONE)
            return set_error(errbuf, errbuf_len, "AOT class hierarchy plan lacks evidence");
        layout = xaot_bundle_find_class_layout_plan(bundle, cls->class_id);
        if (!layout)
            return set_error(errbuf, errbuf_len, "AOT class has no layout plan");
        if (!layout->c_type_name)
            return set_error(errbuf, errbuf_len, "AOT class layout plan has no C type name");
        if (layout->field_start != cls->field_start || layout->field_count != cls->field_count)
            return set_error(errbuf, errbuf_len, "AOT class layout plan mismatches fields");
        if (((layout->flags & XAOT_CLASS_LAYOUT_PREFIX_PARENT) != 0) !=
            (cls->parent_class_id != XG_NO_ID))
            return set_error(errbuf, errbuf_len, "AOT class layout prefix flag mismatches graph");
    }
    if (bundle->nclass_hierarchy_plans != runtime_class_count ||
        bundle->nclass_layout_plans != runtime_class_count)
        return set_error(errbuf, errbuf_len, "AOT class plan count mismatches evidence");

    if (!verify_method_override_graph(ev, errbuf, errbuf_len))
        return false;
    if (!verify_interface_implementor_set(ev, bundle, errbuf, errbuf_len))
        return false;

    for (uint32_t i = 0; i < ev->ncallsites; i++) {
        const XgCallsiteSummary *call = &ev->callsites[i];
        const XaotMethodDispatchPlan *plan;
        if (call->kind != XG_CALL_METHOD && call->kind != XG_CALL_INTERFACE)
            continue;
        expected_dispatch_plans++;
        plan = xaot_bundle_find_method_dispatch_plan(bundle, call->callsite_id);
        if (!plan)
            return set_error(errbuf, errbuf_len, "AOT method callsite has no dispatch plan");
        if (!verify_method_dispatch_plan_rederives(ev, bundle, plan, call, errbuf, errbuf_len))
            return false;
        expected_dispatch_target_cases += plan->target_count;
    }
    if (bundle->nmethod_dispatch_plans != expected_dispatch_plans)
        return set_error(errbuf, errbuf_len, "AOT dispatch plan count mismatches evidence");
    if (bundle->ndispatch_target_cases != expected_dispatch_target_cases)
        return set_error(errbuf, errbuf_len, "AOT dispatch target count mismatches evidence");

    for (uint32_t i = 0; i < bundle->ninterface_use_plans; i++) {
        const XaotInterfaceUsePlan *plan = &bundle->interface_use_plans[i];
        const uint32_t storage_reason_mask =
            XAOT_INTERFACE_USE_REASON_VALUE | XAOT_INTERFACE_USE_REASON_ARRAY |
            XAOT_INTERFACE_USE_REASON_FIELD | XAOT_INTERFACE_USE_REASON_RETURN |
            XAOT_INTERFACE_USE_REASON_CAPTURE | XAOT_INTERFACE_USE_REASON_PARAM;
        if (plan->reason == 0)
            return set_error(errbuf, errbuf_len, "AOT interface-use plan has no reason");
        if (plan->use_site_id == XG_NO_ID) {
            uint32_t storage_reason = plan->reason & storage_reason_mask;
            uint32_t expected_storage_reason =
                verify_interface_object_use_reason_for_interface(ev, plan->interface_id);
            if ((plan->reason & XAOT_INTERFACE_USE_REASON_IMPLEMENTS) != 0 &&
                !verify_has_interface_impl(ev, plan->interface_id, plan->implementor_class_id))
                return set_error(errbuf, errbuf_len,
                                 "AOT interface-use plan has no implements evidence");
            if (storage_reason != 0) {
                uint32_t expected_flags = XAOT_INTERFACE_USE_NEEDS_IFACE_OBJECT;
                if (!verify_has_effective_interface_impl(ev, plan->interface_id,
                                                         plan->implementor_class_id))
                    return set_error(errbuf, errbuf_len,
                                     "AOT interface-use storage plan has no effective evidence");
                if ((storage_reason & ~expected_storage_reason) != 0)
                    return set_error(errbuf, errbuf_len,
                                     "AOT interface-use storage reason does not re-derive");
                if (verify_interface_visible_method_count(ev, plan->interface_id) != 0)
                    expected_flags |= XAOT_INTERFACE_USE_NEEDS_ITABLE;
                if ((plan->flags & expected_flags) != expected_flags)
                    return set_error(errbuf, errbuf_len,
                                     "AOT interface-use storage flags do not re-derive");
            }
        }
        if (plan->use_site_id != XG_NO_ID) {
            const XgCallsiteSummary *call = verify_find_evidence_callsite(ev, plan->use_site_id);
            const XaotMethodDispatchPlan *dispatch =
                xaot_bundle_find_method_dispatch_plan(bundle, plan->use_site_id);
            bool target_found = false;
            if (!call)
                return set_error(errbuf, errbuf_len, "AOT interface-use plan has unknown use-site");
            if (call->kind != XG_CALL_INTERFACE ||
                plan->interface_id != call->receiver_static_interface_id)
                return set_error(errbuf, errbuf_len,
                                 "AOT interface-use plan use-site does not re-derive");
            if ((plan->reason & XAOT_INTERFACE_USE_REASON_VALUE) == 0 ||
                (plan->flags & XAOT_INTERFACE_USE_NEEDS_IFACE_OBJECT) == 0)
                return set_error(errbuf, errbuf_len,
                                 "AOT interface-use plan use-site flags do not re-derive");
            if (!verify_has_effective_interface_impl(ev, plan->interface_id,
                                                     plan->implementor_class_id))
                return set_error(errbuf, errbuf_len,
                                 "AOT interface-use plan has no effective implements evidence");
            if (!dispatch || (dispatch->kind != XAOT_DISPATCH_DIRECT &&
                              dispatch->kind != XAOT_DISPATCH_TYPE_SWITCH &&
                              dispatch->kind != XAOT_DISPATCH_ITABLE))
                return set_error(errbuf, errbuf_len,
                                 "AOT interface-use plan dispatch does not re-derive");
            for (uint16_t ti = 0; ti < dispatch->target_count; ti++) {
                const XaotDispatchTargetCase *target =
                    &bundle->dispatch_target_cases[dispatch->target_start - 1 + ti];
                if (target->callsite_id == plan->use_site_id &&
                    target->receiver_class_id == plan->implementor_class_id) {
                    target_found = true;
                    break;
                }
            }
            if (!target_found)
                return set_error(errbuf, errbuf_len,
                                 "AOT interface-use plan target does not re-derive");
        }
    }

    for (uint32_t i = 0; i < ev->ncallsites; i++) {
        const XgCallsiteSummary *call = &ev->callsites[i];
        const XaotInterfaceAbiPlan *plan;
        bool seen = false;
        if (call->kind != XG_CALL_INTERFACE || call->receiver_static_interface_id == XG_NO_ID)
            continue;
        for (uint32_t j = 0; j < i; j++) {
            const XgCallsiteSummary *prev = &ev->callsites[j];
            if (prev->kind == XG_CALL_INTERFACE &&
                prev->receiver_static_interface_id == call->receiver_static_interface_id) {
                seen = true;
                break;
            }
        }
        if (seen)
            continue;
        expected_interface_abi_plans++;
        plan = xaot_bundle_find_interface_abi_plan(bundle, call->receiver_static_interface_id);
        if (!plan)
            return set_error(errbuf, errbuf_len, "AOT interface callsite has no ABI plan");
        if (!verify_interface_abi_plan_rederives(ev, bundle, plan, errbuf, errbuf_len))
            return false;
    }
    for (uint32_t i = 0; i < ev->ninterface_object_uses; i++) {
        const XgInterfaceObjectUseSummary *use = &ev->interface_object_uses[i];
        const XaotInterfaceAbiPlan *plan;
        bool seen = false;
        if (use->interface_id == XG_NO_ID)
            continue;
        for (uint32_t j = 0; j < ev->ncallsites; j++) {
            const XgCallsiteSummary *call = &ev->callsites[j];
            if (call->kind == XG_CALL_INTERFACE &&
                call->receiver_static_interface_id == use->interface_id) {
                seen = true;
                break;
            }
        }
        for (uint32_t j = 0; !seen && j < i; j++) {
            if (ev->interface_object_uses[j].interface_id == use->interface_id)
                seen = true;
        }
        if (seen)
            continue;
        expected_interface_abi_plans++;
        plan = xaot_bundle_find_interface_abi_plan(bundle, use->interface_id);
        if (!plan)
            return set_error(errbuf, errbuf_len, "AOT interface object use has no ABI plan");
        if (!verify_interface_abi_plan_rederives(ev, bundle, plan, errbuf, errbuf_len))
            return false;
    }
    if (bundle->ninterface_abi_plans != expected_interface_abi_plans)
        return set_error(errbuf, errbuf_len, "AOT interface ABI plan count mismatches evidence");

    for (uint32_t i = 0; i < ev->ncallsites; i++) {
        const XgCallsiteSummary *call = &ev->callsites[i];
        const XaotGenericSpecializationPlan *plan;
        if (call->kind != XG_CALL_INTERFACE)
            continue;
        expected_generic_specialization_plans++;
        plan = xaot_bundle_find_generic_specialization_plan(bundle, call->callsite_id);
        if (!plan)
            return set_error(errbuf, errbuf_len,
                             "AOT interface callsite has no generic specialization plan");
        if (!verify_generic_specialization_plan_rederives(ev, bundle, plan, call, errbuf,
                                                          errbuf_len))
            return false;
    }
    if (bundle->ngeneric_specialization_plans != expected_generic_specialization_plans)
        return set_error(errbuf, errbuf_len,
                         "AOT generic specialization plan count mismatches evidence");

    for (uint32_t i = 0; i < ev->ngeneric_insts; i++) {
        const XgGenericInstSummary *inst = &ev->generic_insts[i];
        const XaotGenericInstantiationPlan *plan;
        expected_generic_instantiation_plans++;
        plan = xaot_bundle_find_generic_instantiation_plan(bundle, inst->generic_inst_id);
        if (!plan)
            return set_error(errbuf, errbuf_len,
                             "AOT generic inst evidence has no instantiation plan");
        if (!verify_generic_instantiation_plan_rederives(plan, inst, errbuf, errbuf_len))
            return false;
    }
    if (bundle->ngeneric_instantiation_plans != expected_generic_instantiation_plans)
        return set_error(errbuf, errbuf_len,
                         "AOT generic instantiation plan count mismatches evidence");

    for (uint32_t i = 0; i < ev->ngeneric_body_uses; i++) {
        const XgGenericBodyUseSummary *use = &ev->generic_body_uses[i];
        const XaotGenericBodyPlan *plan;
        expected_generic_body_plans++;
        plan = xaot_bundle_find_generic_body_plan(bundle, use->use_id);
        if (!plan)
            return set_error(errbuf, errbuf_len, "AOT generic body-use evidence has no body plan");
        if (!verify_generic_body_plan_rederives(ev, plan, use, errbuf, errbuf_len))
            return false;
    }
    if (bundle->ngeneric_body_plans != expected_generic_body_plans)
        return set_error(errbuf, errbuf_len, "AOT generic body plan count mismatches evidence");

    for (uint32_t i = 0; i < ev->ngeneric_storages; i++) {
        const XgGenericStorageSummary *storage = &ev->generic_storages[i];
        const XaotGenericStoragePlan *plan;
        expected_generic_storage_plans++;
        plan = xaot_bundle_find_generic_storage_plan(bundle, storage->storage_id);
        if (!plan)
            return set_error(errbuf, errbuf_len,
                             "AOT generic storage evidence has no storage plan");
        if (!verify_generic_storage_plan_rederives(ev, plan, storage, errbuf, errbuf_len))
            return false;
    }
    if (bundle->ngeneric_storage_plans != expected_generic_storage_plans)
        return set_error(errbuf, errbuf_len, "AOT generic storage plan count mismatches evidence");

    for (uint32_t i = 0; i < ev->ngeneric_code_sizes; i++) {
        const XgGenericCodeSizeSummary *size = &ev->generic_code_sizes[i];
        const XaotGenericCodeSizePlan *plan;
        expected_generic_code_size_plans++;
        plan = xaot_bundle_find_generic_code_size_plan(bundle, size->code_size_id);
        if (!plan)
            return set_error(errbuf, errbuf_len,
                             "AOT generic code-size evidence has no code-size plan");
        if (!verify_generic_code_size_plan_rederives(ev, plan, size, errbuf, errbuf_len))
            return false;
    }
    if (bundle->ngeneric_code_size_plans != expected_generic_code_size_plans)
        return set_error(errbuf, errbuf_len,
                         "AOT generic code-size plan count mismatches evidence");

    for (uint32_t i = 0; i < ev->nderives; i++) {
        const XgDeriveSummary *derive = &ev->derives[i];
        const XaotDerivePlan *plan;
        expected_derive_plans++;
        plan = xaot_bundle_find_derive_plan(bundle, derive->derive_id);
        if (!plan)
            return set_error(errbuf, errbuf_len, "AOT derive evidence has no derive plan");
        if (!verify_derive_plan_rederives(ev, plan, derive, errbuf, errbuf_len))
            return false;
    }
    if (bundle->nderive_plans != expected_derive_plans)
        return set_error(errbuf, errbuf_len, "AOT derive plan count mismatches evidence");

    for (uint32_t i = 0; i < ev->nderives; i++) {
        const XgDeriveSummary *derive = &ev->derives[i];
        const XaotDerivedEqHashPlan *plan;
        bool seen = false;
        if (derive->derive_kind != XG_DERIVE_EQ && derive->derive_kind != XG_DERIVE_HASH)
            continue;
        for (uint32_t j = 0; j < i; j++) {
            const XgDeriveSummary *prev = &ev->derives[j];
            if ((prev->derive_kind == XG_DERIVE_EQ || prev->derive_kind == XG_DERIVE_HASH) &&
                prev->owner_decl_id == derive->owner_decl_id &&
                prev->type_key == derive->type_key) {
                seen = true;
                break;
            }
        }
        if (seen)
            continue;
        expected_derived_eq_hash_plans++;
        plan = xaot_bundle_find_derived_eq_hash_plan(bundle, derive->type_key);
        if (!plan)
            return set_error(errbuf, errbuf_len, "AOT Eq/Hash derive evidence has no plan");
        if (!verify_derived_eq_hash_plan_rederives(ev, plan, derive, errbuf, errbuf_len))
            return false;
    }
    if (bundle->nderived_eq_hash_plans != expected_derived_eq_hash_plans)
        return set_error(errbuf, errbuf_len, "AOT derived Eq/Hash plan count mismatches evidence");

    for (uint32_t i = 0; i < ev->nderives; i++) {
        const XgDeriveSummary *derive = &ev->derives[i];
        const XaotDerivedClonePlan *plan;
        if (derive->derive_kind != XG_DERIVE_CLONE)
            continue;
        expected_derived_clone_plans++;
        plan = xaot_bundle_find_derived_clone_plan(bundle, derive->type_key);
        if (!plan)
            return set_error(errbuf, errbuf_len, "AOT Clone derive evidence has no plan");
        if (!verify_derived_clone_plan_rederives(ev, plan, derive, errbuf, errbuf_len))
            return false;
    }
    if (bundle->nderived_clone_plans != expected_derived_clone_plans)
        return set_error(errbuf, errbuf_len, "AOT derived Clone plan count mismatches evidence");

    for (uint32_t i = 0; i < ev->njson_shapes; i++) {
        const XgJsonShapeSummary *shape = &ev->json_shapes[i];
        const XaotJsonShapePlan *plan;
        expected_json_shape_plans++;
        plan = xaot_bundle_find_json_shape_plan(bundle, shape->json_shape_id);
        if (!plan)
            return set_error(errbuf, errbuf_len, "AOT Json shape evidence has no shape plan");
        if (!verify_json_shape_plan_rederives(plan, shape, errbuf, errbuf_len))
            return false;
    }
    if (bundle->njson_shape_plans != expected_json_shape_plans)
        return set_error(errbuf, errbuf_len, "AOT Json shape plan count mismatches evidence");

    for (uint32_t i = 0; i < ev->njson_accesses; i++) {
        const XgJsonAccessSummary *access = &ev->json_accesses[i];
        const XaotJsonAccessPlan *plan;
        expected_json_access_plans++;
        plan = xaot_bundle_find_json_access_plan(bundle, access->json_access_id);
        if (!plan)
            return set_error(errbuf, errbuf_len, "AOT Json access evidence has no access plan");
        if (!verify_json_access_plan_rederives(ev, plan, access, errbuf, errbuf_len))
            return false;
    }
    if (bundle->njson_access_plans != expected_json_access_plans)
        return set_error(errbuf, errbuf_len, "AOT Json access plan count mismatches evidence");

    for (uint32_t i = 0; i < ev->njson_codecs; i++) {
        const XgJsonCodecSummary *codec = &ev->json_codecs[i];
        const XaotJsonCodecPlan *plan;
        expected_json_codec_plans++;
        plan = xaot_bundle_find_json_codec_plan(bundle, codec->codec_id);
        if (!plan)
            return set_error(errbuf, errbuf_len, "AOT Json codec evidence has no codec plan");
        if (!verify_json_codec_plan_rederives(plan, codec, errbuf, errbuf_len))
            return false;
    }
    if (bundle->njson_codec_plans != expected_json_codec_plans)
        return set_error(errbuf, errbuf_len, "AOT Json codec plan count mismatches evidence");

    for (uint32_t i = 0; i < ev->nrecord_shapes; i++) {
        const XgRecordShapeSummary *shape = &ev->record_shapes[i];
        const XaotRecordShapePlan *plan;
        expected_record_shape_plans++;
        plan = xaot_bundle_find_record_shape_plan(bundle, shape->record_shape_id);
        if (!plan)
            return set_error(errbuf, errbuf_len, "AOT Record shape evidence has no shape plan");
        if (!verify_record_shape_plan_rederives(plan, shape, errbuf, errbuf_len))
            return false;
    }
    if (bundle->nrecord_shape_plans != expected_record_shape_plans)
        return set_error(errbuf, errbuf_len, "AOT Record shape plan count mismatches evidence");

    for (uint32_t i = 0; i < ev->nrecord_accesses; i++) {
        const XgRecordAccessSummary *access = &ev->record_accesses[i];
        const XaotRecordAccessPlan *plan;
        expected_record_access_plans++;
        plan = xaot_bundle_find_record_access_plan(bundle, access->record_access_id);
        if (!plan)
            return set_error(errbuf, errbuf_len, "AOT Record access evidence has no access plan");
        if (!verify_record_access_plan_rederives(ev, plan, access, errbuf, errbuf_len))
            return false;
    }
    if (bundle->nrecord_access_plans != expected_record_access_plans)
        return set_error(errbuf, errbuf_len, "AOT Record access plan count mismatches evidence");

    for (uint32_t i = 0; i < ev->nmap_shapes; i++) {
        const XgMapShapeSummary *shape = &ev->map_shapes[i];
        const XaotMapShapePlan *plan;
        expected_map_shape_plans++;
        plan = xaot_bundle_find_map_shape_plan(bundle, shape->shape_id);
        if (!plan)
            return set_error(errbuf, errbuf_len, "AOT Map/Set shape evidence has no shape plan");
        if (!verify_map_shape_plan_rederives(plan, shape, errbuf, errbuf_len))
            return false;
    }
    if (bundle->nmap_shape_plans != expected_map_shape_plans)
        return set_error(errbuf, errbuf_len, "AOT Map/Set shape plan count mismatches evidence");

    for (uint32_t i = 0; i < ev->nhash_eqs; i++) {
        const XgHashEqSummary *hash_eq = &ev->hash_eqs[i];
        const XaotHashEqPlan *plan;
        expected_hash_eq_plans++;
        plan = xaot_bundle_find_hash_eq_plan(bundle, hash_eq->type_key);
        if (!plan)
            return set_error(errbuf, errbuf_len, "AOT Hash/Eq evidence has no plan");
        if (!verify_hash_eq_plan_rederives(plan, hash_eq, errbuf, errbuf_len))
            return false;
    }
    if (bundle->nhash_eq_plans != expected_hash_eq_plans)
        return set_error(errbuf, errbuf_len, "AOT Hash/Eq plan count mismatches evidence");

    for (uint32_t i = 0; i < ev->nkey_accesses; i++) {
        const XgKeyAccessSummary *access = &ev->key_accesses[i];
        const XaotKeyAccessPlan *plan;
        expected_key_access_plans++;
        plan = xaot_bundle_find_key_access_plan(bundle, access->access_id);
        if (!plan)
            return set_error(errbuf, errbuf_len, "AOT key access evidence has no plan");
        if (!verify_key_access_plan_rederives(ev, plan, access, errbuf, errbuf_len))
            return false;
    }
    if (bundle->nkey_access_plans != expected_key_access_plans)
        return set_error(errbuf, errbuf_len, "AOT key access plan count mismatches evidence");

    for (uint32_t i = 0; i < ev->nsequence_accesses; i++) {
        const XgSequenceAccessSummary *seq = &ev->sequence_accesses[i];
        const XaotSequenceAccessPlan *plan;
        if (seq->access_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT sequence access evidence has no id");
        for (uint32_t j = i + 1; j < ev->nsequence_accesses; j++) {
            if (ev->sequence_accesses[j].access_id == seq->access_id)
                return set_error(errbuf, errbuf_len,
                                 "AOT sequence access evidence id is duplicated");
        }
        expected_sequence_access_plans++;
        plan = xaot_bundle_find_sequence_access_plan(bundle, seq->access_id);
        if (!plan)
            return set_error(errbuf, errbuf_len, "AOT sequence access evidence has no plan");
        if (!verify_sequence_access_plan_rederives(plan, seq, errbuf, errbuf_len))
            return false;
    }
    if (bundle->nsequence_access_plans != expected_sequence_access_plans)
        return set_error(errbuf, errbuf_len, "AOT sequence access plan count mismatches evidence");

    for (uint32_t i = 0; i < ev->ncapacity_ops; i++) {
        const XgCapacityOpSummary *cap = &ev->capacity_ops[i];
        const XaotCapacityPlan *plan;
        if (cap->op_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT capacity evidence has no id");
        for (uint32_t j = i + 1; j < ev->ncapacity_ops; j++) {
            if (ev->capacity_ops[j].op_id == cap->op_id)
                return set_error(errbuf, errbuf_len, "AOT capacity evidence id is duplicated");
        }
        expected_capacity_plans++;
        plan = xaot_bundle_find_capacity_plan(bundle, cap->op_id);
        if (!plan)
            return set_error(errbuf, errbuf_len, "AOT capacity evidence has no plan");
        if (!verify_capacity_plan_rederives(plan, cap, errbuf, errbuf_len))
            return false;
    }
    if (bundle->ncapacity_plans != expected_capacity_plans)
        return set_error(errbuf, errbuf_len, "AOT capacity plan count mismatches evidence");

    for (uint32_t i = 0; i < ev->nbulk_ops; i++) {
        const XgBulkOpSummary *bulk = &ev->bulk_ops[i];
        const XaotBulkPlan *plan;
        if (bulk->op_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT bulk evidence has no id");
        for (uint32_t j = i + 1; j < ev->nbulk_ops; j++) {
            if (ev->bulk_ops[j].op_id == bulk->op_id)
                return set_error(errbuf, errbuf_len, "AOT bulk evidence id is duplicated");
        }
        expected_bulk_plans++;
        plan = xaot_bundle_find_bulk_plan(bundle, bulk->op_id);
        if (!plan)
            return set_error(errbuf, errbuf_len, "AOT bulk evidence has no plan");
        if (!verify_bulk_plan_rederives(plan, bulk, errbuf, errbuf_len))
            return false;
    }
    if (bundle->nbulk_plans != expected_bulk_plans)
        return set_error(errbuf, errbuf_len, "AOT bulk plan count mismatches evidence");

    for (uint32_t i = 0; i < ev->nencoding_ops; i++) {
        const XgEncodingOpSummary *enc = &ev->encoding_ops[i];
        const XaotEncodingPlan *plan;
        if (enc->op_id == XG_NO_ID)
            return set_error(errbuf, errbuf_len, "AOT encoding evidence has no id");
        for (uint32_t j = i + 1; j < ev->nencoding_ops; j++) {
            if (ev->encoding_ops[j].op_id == enc->op_id)
                return set_error(errbuf, errbuf_len, "AOT encoding evidence id is duplicated");
        }
        expected_encoding_plans++;
        plan = xaot_bundle_find_encoding_plan(bundle, enc->op_id);
        if (!plan)
            return set_error(errbuf, errbuf_len, "AOT encoding evidence has no plan");
        if (!verify_encoding_plan_rederives(plan, enc, errbuf, errbuf_len))
            return false;
    }
    if (bundle->nencoding_plans != expected_encoding_plans)
        return set_error(errbuf, errbuf_len, "AOT encoding plan count mismatches evidence");

    for (uint32_t mi = 0; mi < metadata_count; mi++) {
        uint32_t bit = metadata[mi];
        uint32_t body_count = 0;
        uint32_t decl_count = 0;
        uint32_t expected_evidence = 0;
        const XaotMetadataReachabilityPlan *plan;
        for (uint32_t bi = 0; bi < ev->nbodies; bi++) {
            if ((ev->bodies[bi].metadata_use_bits & bit) != 0)
                body_count++;
        }
        for (uint32_t di = 0; di < ev->ndecls; di++) {
            if (bit == XG_METADATA_DERIVE && ev->decls[di].derive_flags != 0)
                decl_count++;
        }
        plan = xaot_bundle_find_metadata_plan(bundle, bit);
        if (body_count == 0 && decl_count == 0) {
            if (plan)
                return set_error(errbuf, errbuf_len, "AOT metadata plan has no evidence");
            continue;
        }
        expected_metadata_plans++;
        if (!plan)
            return set_error(errbuf, errbuf_len, "AOT metadata has no reachability plan");
        if (plan->body_count != body_count || plan->decl_count != decl_count)
            return set_error(errbuf, errbuf_len, "AOT metadata plan count mismatches evidence");
        if (body_count != 0)
            expected_evidence |= XAOT_METADATA_EV_GLOBAL_BODY;
        if (decl_count != 0)
            expected_evidence |= XAOT_METADATA_EV_DECL_ATTRIBUTE;
        if (plan->evidence != expected_evidence ||
            plan->unproven_reason != XAOT_METADATA_UNPROVEN_NONE)
            return set_error(errbuf, errbuf_len, "AOT metadata plan lacks evidence");
        if (plan->profile_action !=
            verify_metadata_profile_action(bundle->global_evidence_plan.profile, bit))
            return set_error(errbuf, errbuf_len, "AOT metadata profile action does not re-derive");
    }
    if (bundle->nmetadata_plans != expected_metadata_plans)
        return set_error(errbuf, errbuf_len, "AOT metadata plan count mismatches evidence");

    for (uint32_t ci = 0; ci < capability_count; ci++) {
        uint32_t cap = capabilities[ci];
        uint32_t body_count = 0;
        uint32_t transfer_count;
        uint32_t expected_evidence = 0;
        const XaotCapabilityPlan *plan;
        for (uint32_t bi = 0; bi < ev->nbodies; bi++) {
            if ((ev->bodies[bi].capability_bits & cap) != 0)
                body_count++;
        }
        transfer_count = verify_capability_transfer_count(bundle, cap);
        plan = xaot_bundle_find_capability_plan(bundle, cap);
        if (body_count == 0 && transfer_count == 0) {
            if (plan)
                return set_error(errbuf, errbuf_len, "AOT capability plan has no evidence");
            continue;
        }
        expected_capability_plans++;
        if (!plan)
            return set_error(errbuf, errbuf_len, "AOT capability has no plan");
        if (plan->body_count != body_count)
            return set_error(errbuf, errbuf_len, "AOT capability body count mismatches evidence");
        if (plan->transfer_count != transfer_count)
            return set_error(errbuf, errbuf_len,
                             "AOT capability transfer count mismatches evidence");
        if (body_count != 0)
            expected_evidence |= XAOT_CAPABILITY_EV_GLOBAL_BODY;
        if (transfer_count != 0)
            expected_evidence |= XAOT_CAPABILITY_EV_TRANSFER_PLAN;
        if (plan->evidence != expected_evidence ||
            plan->unproven_reason != XAOT_CAPABILITY_UNPROVEN_NONE)
            return set_error(errbuf, errbuf_len, "AOT capability plan lacks evidence");
        if (plan->profile_action !=
            verify_capability_profile_action(bundle->global_evidence_plan.profile, cap))
            return set_error(errbuf, errbuf_len,
                             "AOT capability profile action does not re-derive");
    }
    if (bundle->ncapability_plans != expected_capability_plans)
        return set_error(errbuf, errbuf_len, "AOT capability plan count mismatches evidence");

    for (uint32_t si = 0; si < static_data_count; si++) {
        uint32_t bit = static_data[si];
        uint32_t body_count = 0;
        const XaotStaticDataPlan *plan;
        for (uint32_t bi = 0; bi < ev->nbodies; bi++) {
            if ((ev->bodies[bi].static_data_use_bits & bit) != 0)
                body_count++;
        }
        plan = xaot_bundle_find_static_data_plan(bundle, bit);
        if (body_count == 0) {
            if (plan)
                return set_error(errbuf, errbuf_len, "AOT static-data plan has no body evidence");
            continue;
        }
        expected_static_data_plans++;
        if (!plan)
            return set_error(errbuf, errbuf_len, "AOT static-data has no plan");
        if (plan->body_count != body_count)
            return set_error(errbuf, errbuf_len, "AOT static-data body count mismatches evidence");
        if (plan->evidence != XAOT_STATIC_DATA_EV_GLOBAL_BODY ||
            plan->unproven_reason != XAOT_STATIC_DATA_UNPROVEN_NONE)
            return set_error(errbuf, errbuf_len, "AOT static-data plan lacks evidence");
        uint32_t expected_action =
            xaot_static_data_action_for(bundle->global_evidence_plan.profile, bit);
        if (plan->action != expected_action)
            return set_error(errbuf, errbuf_len, "AOT static-data action does not re-derive");
        if (plan->section != xaot_static_data_section_for(bit, expected_action))
            return set_error(errbuf, errbuf_len, "AOT static-data section does not re-derive");
        if (plan->align != xaot_static_data_align_for(bit, expected_action))
            return set_error(errbuf, errbuf_len, "AOT static-data align does not re-derive");
        if (plan->type_hash != xaot_static_data_type_hash_for(bit, expected_action))
            return set_error(errbuf, errbuf_len, "AOT static-data type hash is stale");
        if (plan->data_hash != xaot_static_data_data_hash_for(ev, bit, expected_action))
            return set_error(errbuf, errbuf_len, "AOT static-data data hash is stale");
    }
    if (bundle->nstatic_data_plans != expected_static_data_plans)
        return set_error(errbuf, errbuf_len, "AOT static-data plan count mismatches evidence");

    for (uint32_t li = 0; li < ev->nlink_deps; li++) {
        const XgLinkDependencySummary *dep = &ev->link_deps[li];
        const XaotLinkDependencyPlan *plan;
        if (dep->link_id == XG_NO_ID || dep->kind == 0 || !dep->name[0])
            return set_error(errbuf, errbuf_len, "AOT link dependency evidence is incomplete");
        for (uint32_t prev_i = 0; prev_i < li; prev_i++) {
            const XgLinkDependencySummary *prev = &ev->link_deps[prev_i];
            if (prev->link_id == dep->link_id)
                return set_error(errbuf, errbuf_len, "AOT link dependency id is duplicated");
            if (prev->kind == dep->kind && strcmp(prev->name, dep->name) == 0)
                return set_error(errbuf, errbuf_len, "AOT link dependency evidence is duplicated");
        }
        if (!verify_link_dependency_name_shape(dep, errbuf, errbuf_len))
            return false;
        expected_link_dependency_plans++;
        plan = xaot_bundle_find_link_dependency_plan(bundle, dep->link_id);
        if (!plan)
            return set_error(errbuf, errbuf_len, "AOT link dependency has no plan");
        if (plan->kind != dep->kind || plan->name_id != dep->name_id ||
            strcmp(plan->name, dep->name) != 0)
            return set_error(errbuf, errbuf_len, "AOT link dependency plan mismatches evidence");
        if (plan->evidence != XAOT_LINK_DEP_EV_GLOBAL_SUMMARY ||
            plan->unproven_reason != XAOT_LINK_DEP_UNPROVEN_NONE)
            return set_error(errbuf, errbuf_len, "AOT link dependency plan lacks evidence");
    }
    if (bundle->nlink_dependency_plans != expected_link_dependency_plans)
        return set_error(errbuf, errbuf_len, "AOT link dependency plan count mismatches evidence");

    return true;
}

static bool verify_func_values_have_plans_recursive(const XaotBundle *bundle, const XiFunc *func,
                                                    char *errbuf, size_t errbuf_len) {
    uint32_t bi;
    uint16_t ci;

    if (!func)
        return set_error(errbuf, errbuf_len, "NULL Xi function in AOT value verifier");

    for (bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks[bi];
        const XiPhi *phi;
        uint32_t vi;
        if (!blk)
            continue;
        for (phi = blk->phis; phi; phi = phi->next) {
            if (!xaot_bundle_find_value_plan(bundle, &phi->value))
                return set_error(errbuf, errbuf_len, "Xi phi has no AOT value plan");
        }
        for (vi = 0; vi < blk->nvalues; vi++) {
            if (!xaot_bundle_find_value_plan(bundle, blk->values[vi]))
                return set_error(errbuf, errbuf_len, "Xi value has no AOT value plan");
        }
    }

    for (ci = 0; ci < func->nchildren; ci++) {
        if (!verify_func_values_have_plans_recursive(bundle, func->children[ci], errbuf,
                                                     errbuf_len))
            return false;
    }
    return true;
}

static bool verify_abi_plan(const XaotFuncPlan *plan, char *errbuf, size_t errbuf_len) {
    uint16_t pi;
    if (!plan || !plan->func)
        return set_error(errbuf, errbuf_len, "AOT function plan has no Xi function");
    uint16_t expected_nparams = (uint16_t) (plan->func->nparams + (plan->func->is_vararg ? 1 : 0));
    if (plan->abi.nparams != expected_nparams)
        return set_error(errbuf, errbuf_len, "AOT function ABI parameter count mismatch");
    if (plan->abi.nparams > 0 && !plan->abi.params)
        return set_error(errbuf, errbuf_len, "AOT function ABI missing parameter slots");
    if (plan->abi.kind == XAOT_ABI_NATIVE && plan->abi.boundary_reason != XAOT_BOUNDARY_NONE)
        return set_error(errbuf, errbuf_len, "native AOT ABI unexpectedly has a boundary reason");
    if (plan->abi.ret.cls != XAOT_ARG_VOID && !plan->abi.ret.c_type)
        return set_error(errbuf, errbuf_len, "AOT function ABI return slot missing C type");
    for (pi = 0; pi < plan->abi.nparams; pi++) {
        if (!plan->abi.params[pi].c_type)
            return set_error(errbuf, errbuf_len, "AOT function ABI parameter slot missing C type");
    }
    return true;
}

static bool storage_reps_equal(XaotValueRep a, XaotValueRep b) {
    return xaot_value_reps_equal(a, b);
}

static bool verify_direct_call_arg_step(const XaotBundle *bundle, const XaotBoundaryStep *step,
                                        char *errbuf, size_t errbuf_len) {
    const XaotFuncPlan *target_plan;
    const XaotValuePlan *arg_plan;

    if (!step->target_func)
        return set_error(errbuf, errbuf_len, "AOT direct call argument step has no target");
    if (!step->value ||
        (step->value->op != XI_CALL && step->value->op != XI_CALL_METHOD &&
         step->value->op != XI_CALL_METHOD_DIRECT) ||
        !step->input)
        return set_error(errbuf, errbuf_len, "AOT direct call argument step has bad site/input");
    if (step->arg_index == UINT16_MAX)
        return set_error(errbuf, errbuf_len, "AOT direct call argument step has no arg index");
    if (step->reason != XAOT_BOUNDARY_DIRECT_CALL)
        return set_error(errbuf, errbuf_len, "AOT direct call argument step has wrong reason");

    target_plan = xaot_bundle_find_func_plan(bundle, step->target_func);
    if (!target_plan)
        return set_error(errbuf, errbuf_len, "AOT direct call argument target has no ABI plan");
    if (step->arg_index >= target_plan->abi.nparams || !target_plan->abi.params)
        return set_error(errbuf, errbuf_len, "AOT direct call argument index is out of range");
    arg_plan = xaot_bundle_find_value_plan(bundle, step->input);
    if (!arg_plan)
        return set_error(errbuf, errbuf_len, "AOT direct call argument input has no value plan");
    if (!storage_reps_equal(step->from_rep, arg_plan->rep))
        return set_error(errbuf, errbuf_len, "AOT direct call argument from-rep mismatch");
    if (!storage_reps_equal(step->to_rep, target_plan->abi.params[step->arg_index].rep))
        return set_error(errbuf, errbuf_len, "AOT direct call argument to-rep mismatch");
    if (storage_reps_equal(step->from_rep, step->to_rep))
        return set_error(errbuf, errbuf_len, "AOT direct call argument step is a no-op");
    return true;
}

static bool verify_direct_call_ret_step(const XaotBundle *bundle, const XaotBoundaryStep *step,
                                        char *errbuf, size_t errbuf_len) {
    const XaotFuncPlan *target_plan;
    const XaotValuePlan *call_plan;

    if (!step->target_func)
        return set_error(errbuf, errbuf_len, "AOT direct call return step has no target");
    if (!step->value ||
        (step->value->op != XI_CALL && step->value->op != XI_CALL_METHOD &&
         step->value->op != XI_CALL_METHOD_DIRECT) ||
        step->input)
        return set_error(errbuf, errbuf_len, "AOT direct call return step has bad site/input");
    if (step->arg_index != UINT16_MAX)
        return set_error(errbuf, errbuf_len, "AOT direct call return step has arg index");
    if (step->reason != XAOT_BOUNDARY_DIRECT_CALL)
        return set_error(errbuf, errbuf_len, "AOT direct call return step has wrong reason");

    target_plan = xaot_bundle_find_func_plan(bundle, step->target_func);
    if (!target_plan)
        return set_error(errbuf, errbuf_len, "AOT direct call return target has no ABI plan");
    call_plan = xaot_bundle_find_value_plan(bundle, step->value);
    if (!call_plan)
        return set_error(errbuf, errbuf_len, "AOT direct call return site has no value plan");
    if (!storage_reps_equal(step->from_rep, target_plan->abi.ret.rep))
        return set_error(errbuf, errbuf_len, "AOT direct call return from-rep mismatch");
    if (!storage_reps_equal(step->to_rep, call_plan->rep))
        return set_error(errbuf, errbuf_len, "AOT direct call return to-rep mismatch");
    if (storage_reps_equal(step->from_rep, step->to_rep))
        return set_error(errbuf, errbuf_len, "AOT direct call return step is a no-op");
    return true;
}

static bool verify_boundary_step(const XaotBoundaryStep *step, char *errbuf, size_t errbuf_len) {
    if (!step)
        return set_error(errbuf, errbuf_len, "AOT boundary step is NULL");
    if (!step->func)
        return set_error(errbuf, errbuf_len, "AOT boundary step has no function");
    if (step->reason == XAOT_BOUNDARY_NONE)
        return set_error(errbuf, errbuf_len, "AOT boundary step has no reason");
    if (step->kind == XAOT_BOUNDARY_STEP_VALUE_REP) {
        if (!step->value || !step->input)
            return set_error(errbuf, errbuf_len, "AOT value boundary step has no value/input");
        if (step->value->op != XI_BOX && step->value->op != XI_UNBOX)
            return set_error(errbuf, errbuf_len, "AOT value boundary step is not box/unbox");
        if (step->value->op == XI_BOX && step->reason != XAOT_BOUNDARY_BOX)
            return set_error(errbuf, errbuf_len, "AOT box boundary has wrong reason");
        if (step->value->op == XI_UNBOX && step->reason != XAOT_BOUNDARY_UNBOX)
            return set_error(errbuf, errbuf_len, "AOT unbox boundary has wrong reason");
    } else if (step->kind == XAOT_BOUNDARY_STEP_FUNC_ABI) {
        if (step->value || step->input)
            return set_error(errbuf, errbuf_len, "AOT function boundary unexpectedly has value");
        if (step->target_func || step->arg_index != UINT16_MAX)
            return set_error(errbuf, errbuf_len, "AOT function boundary has call-site key");
    } else if (step->kind == XAOT_BOUNDARY_STEP_DIRECT_CALL_ARG) {
        return true;
    } else if (step->kind == XAOT_BOUNDARY_STEP_DIRECT_CALL_RET) {
        return true;
    } else {
        return set_error(errbuf, errbuf_len, "AOT boundary step has unknown kind");
    }
    return true;
}

static bool verify_direct_call_boundaries(const XaotBundle *bundle, const XiFunc *func,
                                          const XiValue *call, char *errbuf, size_t errbuf_len) {
    const XiFunc *target;
    const XaotFuncPlan *target_plan;
    const XaotValuePlan *call_plan;
    uint16_t first_arg;
    uint16_t call_arg_count;
    uint16_t a;

    if (!bundle || !func || !call)
        return true;
    if (call->op != XI_CALL && call->op != XI_CALL_METHOD && call->op != XI_CALL_METHOD_DIRECT)
        return true;
    target = xaot_boundary_resolve_direct_call_target(bundle, func, call, &first_arg);
    if (!target)
        return true;
    target_plan = xaot_bundle_find_func_plan(bundle, target);
    if (!target_plan)
        return set_error(errbuf, errbuf_len, "AOT direct call target has no function plan");
    call_arg_count = call->nargs > first_arg ? (uint16_t) (call->nargs - first_arg) : 0;
    /* Vararg: only the fixed parameters map to ABI slots; trailing args are
     * collected into the rest Array at the call site (boxed to tagged), so they
     * carry no per-arg boundary step and may exceed the fixed count. */
    uint16_t verify_argc = target->is_vararg ? target->nparams : call_arg_count;
    if (!target->is_vararg && call_arg_count > target_plan->abi.nparams)
        return set_error(errbuf, errbuf_len, "AOT direct call has more args than target ABI");
    for (a = first_arg; a < first_arg + verify_argc; a++) {
        const XiValue *arg = call->args[a];
        const XaotValuePlan *arg_plan;
        const XaotBoundaryStep *step;
        if (!arg)
            return set_error(errbuf, errbuf_len, "AOT direct call has NULL argument");
        arg_plan = xaot_bundle_find_value_plan(bundle, arg);
        if (!arg_plan)
            return set_error(errbuf, errbuf_len, "AOT direct call arg has no value plan");
        if (storage_reps_equal(arg_plan->rep,
                               xaot_abi_slot_value_rep(&target_plan->abi.params[a - first_arg])))
            continue;
        step = xaot_bundle_find_boundary_step_ex(bundle, XAOT_BOUNDARY_STEP_DIRECT_CALL_ARG, func,
                                                 call, arg, target, (uint16_t) (a - first_arg));
        if (!step)
            return set_error(errbuf, errbuf_len, "AOT direct call arg conversion has no step");
    }
    if (target_plan->abi.ret.cls == XAOT_ARG_VOID)
        return true;
    call_plan = xaot_bundle_find_value_plan(bundle, call);
    if (!call_plan)
        return set_error(errbuf, errbuf_len, "AOT direct call result has no value plan");
    if (storage_reps_equal(xaot_abi_slot_value_rep(&target_plan->abi.ret), call_plan->rep))
        return true;
    if (!xaot_bundle_find_boundary_step_ex(bundle, XAOT_BOUNDARY_STEP_DIRECT_CALL_RET, func, call,
                                           NULL, target, UINT16_MAX))
        return set_error(errbuf, errbuf_len, "AOT direct call return conversion has no step");
    return true;
}

static bool verify_func_boundaries_recursive(const XaotBundle *bundle, const XiFunc *func,
                                             char *errbuf, size_t errbuf_len) {
    uint32_t bi;
    uint16_t ci;
    const XaotFuncPlan *plan;

    if (!func)
        return set_error(errbuf, errbuf_len, "NULL Xi function in AOT boundary verifier");

    plan = xaot_bundle_find_func_plan(bundle, func);
    if (!plan)
        return set_error(errbuf, errbuf_len, "Xi function has no AOT function plan");
    if (plan->abi.boundary_reason != XAOT_BOUNDARY_NONE &&
        !xaot_bundle_find_boundary_step(bundle, XAOT_BOUNDARY_STEP_FUNC_ABI, func, NULL, NULL))
        return set_error(errbuf, errbuf_len, "AOT function ABI boundary has no step");

    for (bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks[bi];
        uint32_t vi;
        if (!blk)
            continue;
        for (vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *value = blk->values[vi];
            if (!value)
                continue;
            if (value->op == XI_BOX || value->op == XI_UNBOX) {
                const XiValue *input;
                if (value->nargs < 1 || !value->args || !value->args[0])
                    return set_error(errbuf, errbuf_len, "AOT box/unbox boundary has no input");
                input = value->args[0];
                if (!xaot_bundle_find_boundary_step(bundle, XAOT_BOUNDARY_STEP_VALUE_REP, func,
                                                    value, input))
                    return set_error(errbuf, errbuf_len, "AOT box/unbox boundary has no step");
            }
            if ((value->op == XI_CALL || value->op == XI_CALL_METHOD ||
                 value->op == XI_CALL_METHOD_DIRECT) &&
                !verify_direct_call_boundaries(bundle, func, value, errbuf, errbuf_len))
                return false;
        }
    }

    for (ci = 0; ci < func->nchildren; ci++) {
        if (!verify_func_boundaries_recursive(bundle, func->children[ci], errbuf, errbuf_len))
            return false;
    }
    return true;
}

static bool verify_func_closure_plans_recursive(const XaotBundle *bundle, const XiFunc *func,
                                                uint32_t *out_count, char *errbuf,
                                                size_t errbuf_len) {
    uint32_t bi;
    uint16_t ci;

    if (!func)
        return set_error(errbuf, errbuf_len, "NULL Xi function in AOT closure verifier");

    for (bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks[bi];
        uint32_t vi;
        if (!blk)
            continue;
        for (vi = 0; vi < blk->nvalues; vi++) {
            XaotClosurePlan derived;
            const XiValue *value = blk->values[vi];
            if (!xaot_prepare_closure_plan_for_value(func, value, &derived))
                continue;
            if (out_count)
                (*out_count)++;
            if (!xaot_bundle_find_closure_plan(bundle, value))
                return set_error(errbuf, errbuf_len, "AOT closure allocation has no plan");
        }
    }

    for (ci = 0; ci < func->nchildren; ci++) {
        if (!verify_func_closure_plans_recursive(bundle, func->children[ci], out_count, errbuf,
                                                 errbuf_len))
            return false;
    }
    return true;
}

static bool verify_func_allocation_plans_recursive(const XaotBundle *bundle, const XiFunc *func,
                                                   uint32_t *out_count, char *errbuf,
                                                   size_t errbuf_len) {
    uint32_t bi;
    uint16_t ci;

    if (!func)
        return set_error(errbuf, errbuf_len, "NULL Xi function in AOT allocation verifier");

    for (bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks[bi];
        uint32_t vi;
        if (!blk)
            continue;
        for (vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *value = blk->values[vi];
            if (!verify_allocation_plan_candidate(value))
                continue;
            if (out_count)
                (*out_count)++;
            if (!xaot_bundle_find_allocation_plan(bundle, value))
                return set_error(errbuf, errbuf_len, "AOT stack allocation has no plan");
        }
    }

    for (ci = 0; ci < func->nchildren; ci++) {
        if (!verify_func_allocation_plans_recursive(bundle, func->children[ci], out_count, errbuf,
                                                    errbuf_len))
            return false;
    }
    return true;
}

static bool verify_func_transfer_plans_recursive(const XaotBundle *bundle, const XiFunc *func,
                                                 uint32_t *out_count, char *errbuf,
                                                 size_t errbuf_len) {
    uint32_t bi;
    uint16_t ci;

    if (!func)
        return set_error(errbuf, errbuf_len, "NULL Xi function in AOT transfer verifier");

    for (bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks[bi];
        uint32_t vi;
        if (!blk)
            continue;
        for (vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *site = blk->values[vi];
            if (!site)
                continue;
            if (site->op == XI_GO || site->op == XI_THREAD_SPAWN) {
                for (uint16_t ai = 1; ai < site->nargs; ai++) {
                    XaotTransferPlan derived;
                    uint16_t transfer_index = (uint16_t) (ai - 1);
                    if (!xaot_prepare_transfer_plan_for_site(func, site, transfer_index, &derived))
                        continue;
                    if (out_count)
                        (*out_count)++;
                    if (!xaot_bundle_find_transfer_plan(bundle, site, transfer_index))
                        return set_error(errbuf, errbuf_len, "AOT transfer boundary has no plan");
                }
            } else {
                XaotTransferPlan derived;
                if (!xaot_prepare_transfer_plan_for_site(func, site, 0, &derived))
                    continue;
                if (out_count)
                    (*out_count)++;
                if (!xaot_bundle_find_transfer_plan(bundle, site, 0))
                    return set_error(errbuf, errbuf_len, "AOT transfer boundary has no plan");
            }
        }
    }

    for (ci = 0; ci < func->nchildren; ci++) {
        if (!verify_func_transfer_plans_recursive(bundle, func->children[ci], out_count, errbuf,
                                                  errbuf_len))
            return false;
    }
    return true;
}

XR_FUNC bool xaot_verify_bundle(const XaotBundle *bundle, XaotVerifyMode mode, char *errbuf,
                                size_t errbuf_len) {
    uint32_t mi;
    uint32_t fi;
    uint32_t allocation_count = 0;
    uint32_t closure_count = 0;
    uint32_t transfer_count = 0;

    (void) mode;
    if (!bundle)
        return set_error(errbuf, errbuf_len, "NULL AOT bundle");
    if (!bundle->modules || bundle->nmodules == 0)
        return set_error(errbuf, errbuf_len, "AOT bundle has no modules");
    if (bundle->entry_module >= bundle->nmodules)
        return set_error(errbuf, errbuf_len, "AOT bundle entry module is out of range");
    if (bundle->nfunc_plans == 0)
        return set_error(errbuf, errbuf_len, "AOT bundle has no function plans");
    if (!verify_global_evidence_plan(bundle, errbuf, errbuf_len))
        return false;

    for (mi = 0; mi < bundle->nmodules; mi++) {
        const XiModule *mod = bundle->modules[mi];
        if (!mod || !mod->init)
            return set_error(errbuf, errbuf_len, "AOT bundle contains an invalid module");
        if (!verify_func_has_plan_recursive(bundle, mod->init, errbuf, errbuf_len))
            return false;
        if (!verify_func_values_have_plans_recursive(bundle, mod->init, errbuf, errbuf_len))
            return false;
        if (!verify_func_boundaries_recursive(bundle, mod->init, errbuf, errbuf_len))
            return false;
        if (!verify_func_allocation_plans_recursive(bundle, mod->init, &allocation_count, errbuf,
                                                    errbuf_len))
            return false;
        if (!verify_func_closure_plans_recursive(bundle, mod->init, &closure_count, errbuf,
                                                 errbuf_len))
            return false;
        if (!verify_func_transfer_plans_recursive(bundle, mod->init, &transfer_count, errbuf,
                                                  errbuf_len))
            return false;
    }
    if (allocation_count != bundle->nallocation_plans)
        return set_error(errbuf, errbuf_len, "AOT allocation plan count does not match IR");
    if (closure_count != bundle->nclosure_plans)
        return set_error(errbuf, errbuf_len, "AOT closure plan count does not match IR");
    if (transfer_count != bundle->ntransfer_plans)
        return set_error(errbuf, errbuf_len, "AOT transfer plan count does not match IR");

    for (fi = 0; fi < bundle->nfunc_plans; fi++) {
        if (!verify_abi_plan(&bundle->func_plans[fi], errbuf, errbuf_len))
            return false;
    }
    for (fi = 0; fi < bundle->nvalue_plans; fi++) {
        if (!verify_value_plan(&bundle->value_plans[fi], errbuf, errbuf_len))
            return false;
    }
    for (fi = 0; fi < bundle->ncontainer_plans; fi++) {
        if (!verify_container_plan(&bundle->container_plans[fi], errbuf, errbuf_len))
            return false;
    }
    for (fi = 0; fi < bundle->nenum_plans; fi++) {
        if (!verify_enum_plan(bundle, &bundle->enum_plans[fi], errbuf, errbuf_len))
            return false;
    }
    for (fi = 0; fi < bundle->narray_storage_plans; fi++) {
        if (!verify_array_storage_plan(bundle, &bundle->array_storage_plans[fi], errbuf,
                                       errbuf_len))
            return false;
    }
    for (fi = 0; fi < bundle->narray_cache_plans; fi++) {
        if (!verify_array_cache_plan(bundle, &bundle->array_cache_plans[fi], errbuf, errbuf_len))
            return false;
    }
    for (fi = 0; fi < bundle->narray_class_field_alloc_plans; fi++) {
        if (!verify_array_class_field_alloc_plan(bundle, &bundle->array_class_field_alloc_plans[fi],
                                                 errbuf, errbuf_len))
            return false;
    }
    for (fi = 0; fi < bundle->nfunc_attr_plans; fi++) {
        if (!verify_func_attr_plan(bundle, &bundle->func_attr_plans[fi], errbuf, errbuf_len))
            return false;
    }
    for (fi = 0; fi < bundle->nbounds_plans; fi++) {
        if (!verify_bounds_plan(bundle, &bundle->bounds_plans[fi], errbuf, errbuf_len))
            return false;
    }
    for (fi = 0; fi < bundle->nspan_access_plans; fi++) {
        if (!verify_span_access_plan(bundle, &bundle->span_access_plans[fi], errbuf, errbuf_len))
            return false;
    }
    for (fi = 0; fi < bundle->nalias_plans; fi++) {
        if (!verify_alias_plan(bundle, &bundle->alias_plans[fi], errbuf, errbuf_len))
            return false;
    }
    for (fi = 0; fi < bundle->nallocation_plans; fi++) {
        if (!verify_allocation_plan(bundle, &bundle->allocation_plans[fi], errbuf, errbuf_len))
            return false;
    }
    for (fi = 0; fi < bundle->nclosure_plans; fi++) {
        if (!verify_closure_plan(bundle, &bundle->closure_plans[fi], errbuf, errbuf_len))
            return false;
    }
    for (fi = 0; fi < bundle->ntransfer_plans; fi++) {
        if (!verify_transfer_plan(bundle, &bundle->transfer_plans[fi], errbuf, errbuf_len))
            return false;
    }
    for (fi = 0; fi < bundle->nboundary_steps; fi++) {
        if (!verify_boundary_step(&bundle->boundary_steps[fi], errbuf, errbuf_len))
            return false;
        if (bundle->boundary_steps[fi].kind == XAOT_BOUNDARY_STEP_DIRECT_CALL_ARG &&
            !verify_direct_call_arg_step(bundle, &bundle->boundary_steps[fi], errbuf, errbuf_len))
            return false;
        if (bundle->boundary_steps[fi].kind == XAOT_BOUNDARY_STEP_DIRECT_CALL_RET &&
            !verify_direct_call_ret_step(bundle, &bundle->boundary_steps[fi], errbuf, errbuf_len))
            return false;
    }
    return true;
}
