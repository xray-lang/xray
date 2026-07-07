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
#include "../ir/xi_effect.h"
#include "../runtime/value/xstruct_layout.h"
#include <stdio.h>
#include <string.h>

static bool set_error(char *errbuf, size_t errbuf_len, const char *msg) {
    if (errbuf && errbuf_len > 0) {
        snprintf(errbuf, errbuf_len, "%s", msg ? msg : "AOT verifier error");
    }
    return false;
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

/* Re-derive the effect evidence behind a function attribute plan.
 * CONST must touch no memory at all; PURE must never write / throw /
 * suspend. A stale or wrong plan would make the C compiler CSE calls
 * with observable effects, so any mismatch is a hard fail. */
static bool verify_func_attr_plan(const XaotBundle *bundle, const XaotFuncAttrPlan *plan,
                                  char *errbuf, size_t errbuf_len) {
    uint32_t bi, vi;
    bool reads_mem;

    if (!bundle || !plan)
        return set_error(errbuf, errbuf_len, "AOT function attribute plan is NULL");
    if (!plan->func)
        return set_error(errbuf, errbuf_len, "AOT function attribute plan lacks func");
    if (plan->flags != XAOT_FN_ATTR_CONST && plan->flags != XAOT_FN_ATTR_PURE)
        return set_error(errbuf, errbuf_len, "AOT function attribute plan has invalid flags");
    if (!xaot_bundle_find_func_plan(bundle, plan->func))
        return set_error(errbuf, errbuf_len, "AOT function attribute plan func has no func plan");

    reads_mem = plan->func->ncaptures > 0;
    for (bi = 0; bi < plan->func->nblocks; bi++) {
        const XiBlock *blk = plan->func->blocks[bi];
        if (!blk)
            continue;
        for (vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            if (v->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND |
                            XI_FLAG_WRITES_MEM))
                return set_error(errbuf, errbuf_len,
                                 "AOT function attribute plan func has effectful value");
            switch ((XiOp) v->op) {
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
            if (xi_op_allocates(v->op))
                return set_error(errbuf, errbuf_len, "AOT function attribute plan func allocates");
            if (v->flags & XI_FLAG_READS_MEM)
                reads_mem = true;
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
    cache_plan = xaot_bundle_find_array_cache_plan(bundle, plan->value);
    if (!cache_plan)
        return set_error(errbuf, errbuf_len, "AOT alias plan has no backing array cache plan");
    if (plan->evidence != xaot_prepare_array_cache_alias_evidence(bundle, plan->func, cache_plan))
        return set_error(errbuf, errbuf_len, "AOT alias plan evidence does not re-derive");
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
                                                  uint32_t signature_key) {
    if (!ev || !cls || cls->method_start == 0 || name_id == 0)
        return NULL;
    for (uint32_t i = 0; i < cls->method_count; i++) {
        uint32_t idx = cls->method_start - 1 + i;
        const XgMethodSummary *method = idx < ev->nmethods ? &ev->methods[idx] : NULL;
        if (method && method->owner_class_id == cls->class_id &&
            xg_verify_method_participates_in_override(method) && method->name_id == name_id &&
            method->signature_key == signature_key)
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
    const XgGlobalEvidence *ev, XgClassId class_id, uint32_t name_id, uint32_t signature_key) {
    const XgClassSummary *cls = verify_find_evidence_class(ev, class_id);
    uint32_t depth = 0;
    while (cls && depth++ < 64) {
        const XgMethodSummary *method =
            verify_find_evidence_method_by_signature_in_class(ev, cls, name_id, signature_key);
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
        method =
            verify_find_evidence_method_by_signature_in_class(ev, parent, name_id, signature_key);
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

static bool verify_method_override_graph(const XgGlobalEvidence *ev, char *errbuf,
                                         size_t errbuf_len) {
    if (!ev)
        return set_error(errbuf, errbuf_len, "AOT global evidence method verifier has no evidence");

    for (uint32_t i = 0; i < ev->nmethods; i++) {
        const XgMethodSummary *method = &ev->methods[i];
        const XgClassSummary *owner;
        const XgMethodSummary *expected_parent = NULL;
        XgMethodId expected_override_of = XG_NO_ID;
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

        if (method->override_of != expected_override_of)
            return set_error(errbuf, errbuf_len,
                             "AOT global evidence method override_of does not re-derive");
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

    if (bundle->ninterface_use_plans != ev->ninterface_impls)
        return set_error(errbuf, errbuf_len, "AOT interface-use plan count mismatches evidence");
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
                if (call->receiver_static_class_id == XG_NO_ID || call->method_id == XG_NO_ID ||
                    call->method_name_id == 0 || call->method_signature_key == 0)
                    return set_error(errbuf, errbuf_len,
                                     "AOT global evidence method callsite identity is stale");
                {
                    const XgMethodSummary *target_method =
                        verify_find_evidence_method_by_signature_in_hierarchy(
                            ev, call->receiver_static_class_id, call->method_name_id,
                            call->method_signature_key);
                    if (!target_method || target_method->method_id != call->method_id)
                        return set_error(
                            errbuf, errbuf_len,
                            "AOT global evidence method callsite target does not re-derive");
                }
                break;
            case XG_CALL_INTERFACE:
                if (call->receiver_static_interface_id == XG_NO_ID || call->method_name_id == 0 ||
                    call->method_signature_key == 0)
                    return set_error(errbuf, errbuf_len,
                                     "AOT global evidence interface callsite identity is stale");
                if (!verify_find_evidence_decl_by_kind_name(ev, XG_DECL_INTERFACE,
                                                            call->receiver_static_interface_id))
                    return set_error(
                        errbuf, errbuf_len,
                        "AOT global evidence interface callsite declaration is missing");
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
                if (call->method_id == XG_NO_ID || call->method_name_id == 0)
                    return set_error(errbuf, errbuf_len,
                                     "AOT global evidence native callsite identity is stale");
                break;
            case XG_CALL_EXTERN:
                if (call->method_id == XG_NO_ID || call->method_name_id == 0)
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
        if (target->callsite_id != plan->callsite_id ||
            target->receiver_class_id != expected_classes[i] ||
            target->method_id != expected_methods[i] || target->evidence != plan->evidence)
            return set_error(errbuf, errbuf_len,
                             expected_kind == XAOT_DISPATCH_TYPE_SWITCH
                                 ? "AOT dispatch type-switch targets do not re-derive"
                                 : "AOT dispatch direct target does not re-derive");
        if (!verify_find_evidence_class(ev, target->receiver_class_id) ||
            !verify_find_evidence_method_by_id(ev, target->method_id))
            return set_error(errbuf, errbuf_len, "AOT dispatch target is missing");
    }
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
            for (uint32_t i = 0; i < ev->ninterface_impls; i++) {
                const XgInterfaceImplSummary *impl = &ev->interface_impls[i];
                const XgMethodSummary *target_method;
                if (impl->interface_id != call->receiver_static_interface_id)
                    continue;
                implementor_count++;
                target_method = verify_find_evidence_method_by_signature_in_hierarchy(
                    ev, impl->implementor_class_id, call->method_name_id,
                    call->method_signature_key);
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
        }
    }

    expected_method_id = method ? method->method_id : call->method_id;
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
    if (plan->dispatch_slot != UINT32_MAX)
        return set_error(errbuf, errbuf_len, "AOT dispatch plan slot is not evidence-derived yet");
    if (!verify_dispatch_target_anchor_rederives(ev, bundle, plan, expected_kind, expected_classes,
                                                 expected_methods, expected_target_count, errbuf,
                                                 errbuf_len))
        return false;
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

static uint32_t verify_static_data_action(uint32_t profile, uint32_t static_data) {
    if (profile == XG_BUILD_FREESTANDING && static_data == XG_STATIC_DATA_RUNTIME_INIT)
        return XAOT_STATIC_DATA_ACTION_REJECT;
    if (static_data == XG_STATIC_DATA_RUNTIME_INIT)
        return XAOT_STATIC_DATA_ACTION_RUNTIME_INIT;
    return XAOT_STATIC_DATA_ACTION_MATERIALIZE;
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

    if (!verify_body_summary_ranges(ev, errbuf, errbuf_len))
        return false;

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
        if (plan->reason == 0)
            return set_error(errbuf, errbuf_len, "AOT interface-use plan has no reason");
        if (plan->use_site_id == XG_NO_ID &&
            !verify_has_interface_impl(ev, plan->interface_id, plan->implementor_class_id))
            return set_error(errbuf, errbuf_len,
                             "AOT interface-use plan has no implements evidence");
        if (plan->use_site_id != XG_NO_ID && !verify_find_evidence_callsite(ev, plan->use_site_id))
            return set_error(errbuf, errbuf_len, "AOT interface-use plan has unknown use-site");
    }

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
            if (bit == XG_METADATA_DERIVE && (ev->decls[di].flags & XG_DECL_DERIVE) != 0)
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
        if (plan->action != verify_static_data_action(bundle->global_evidence_plan.profile, bit))
            return set_error(errbuf, errbuf_len, "AOT static-data action does not re-derive");
    }
    if (bundle->nstatic_data_plans != expected_static_data_plans)
        return set_error(errbuf, errbuf_len, "AOT static-data plan count mismatches evidence");

    for (uint32_t li = 0; li < ev->nlink_deps; li++) {
        const XgLinkDependencySummary *dep = &ev->link_deps[li];
        const XaotLinkDependencyPlan *plan;
        if (dep->link_id == XG_NO_ID || dep->kind == 0 || !dep->name[0])
            return set_error(errbuf, errbuf_len, "AOT link dependency evidence is incomplete");
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
        if (!verify_func_closure_plans_recursive(bundle, mod->init, &closure_count, errbuf,
                                                 errbuf_len))
            return false;
        if (!verify_func_transfer_plans_recursive(bundle, mod->init, &transfer_count, errbuf,
                                                  errbuf_len))
            return false;
    }
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
