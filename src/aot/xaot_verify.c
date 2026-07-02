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
        if (plan->type->kind != XR_KIND_ARRAY && plan->type->kind != XR_KIND_SPAN &&
            plan->type->kind != XR_KIND_FIXED_ARRAY)
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
    const XrStructLayout *layout;
    const XrStructFieldLayout *field;

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

XR_FUNC bool xaot_verify_bundle(const XaotBundle *bundle, XaotVerifyMode mode, char *errbuf,
                                size_t errbuf_len) {
    uint32_t mi;
    uint32_t fi;

    (void) mode;
    if (!bundle)
        return set_error(errbuf, errbuf_len, "NULL AOT bundle");
    if (!bundle->modules || bundle->nmodules == 0)
        return set_error(errbuf, errbuf_len, "AOT bundle has no modules");
    if (bundle->entry_module >= bundle->nmodules)
        return set_error(errbuf, errbuf_len, "AOT bundle entry module is out of range");
    if (bundle->nfunc_plans == 0)
        return set_error(errbuf, errbuf_len, "AOT bundle has no function plans");

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
    }

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
    for (fi = 0; fi < bundle->nalias_plans; fi++) {
        if (!verify_alias_plan(bundle, &bundle->alias_plans[fi], errbuf, errbuf_len))
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
