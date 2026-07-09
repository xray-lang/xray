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
#include "../frontend/parser/xtype_ref.h"
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

static bool reserve_plan_array(void **items, uint32_t *cap, uint32_t needed, size_t elem_size,
                               uint32_t initial_cap) {
    uint32_t new_cap;
    void *new_items;

    if (!items || !cap || elem_size == 0)
        return false;
    if (*cap >= needed)
        return true;
    new_cap = *cap < initial_cap ? initial_cap : *cap;
    while (new_cap < needed) {
        if (new_cap > UINT32_MAX / 2)
            return false;
        new_cap *= 2;
    }
    if ((size_t) new_cap > SIZE_MAX / elem_size)
        return false;
    new_items = xr_realloc(*items, (size_t) new_cap * elem_size);
    if (!new_items)
        return false;
    *items = new_items;
    *cap = new_cap;
    return true;
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

static bool xaot_enum_plan_payloads_fit_compact_aggregate(const XaotEnumPlan *plan) {
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

static void xaot_enum_plan_finalize_scalarization(XaotEnumPlan *plan) {
    if (!plan)
        return;
    plan->scalar_payload_cap = XAOT_ENUM_SCALAR_PAYLOAD_CAP;
    plan->scalar_action = XAOT_ENUM_SCALAR_RUNTIME_AGGREGATE;
    plan->scalar_evidence = 0;
    if (plan->layout_id != 0)
        plan->scalar_evidence |= XAOT_ENUM_SCALAR_EV_LAYOUT_ID;
    if (!xaot_enum_plan_payloads_fit_compact_aggregate(plan))
        return;
    plan->scalar_action = XAOT_ENUM_SCALAR_COMPACT_AGGREGATE;
    plan->scalar_evidence |= XAOT_ENUM_SCALAR_EV_PAYLOAD_BOUND | XAOT_ENUM_SCALAR_EV_TYPED_UNION;
    if (plan->type_arg_count > 0)
        plan->scalar_evidence |= XAOT_ENUM_SCALAR_EV_CONCRETE_TYPES;
}

static void xaot_class_layout_plan_free(XaotClassLayoutPlan *plan) {
    if (!plan)
        return;
    xr_free(plan->c_type_name);
    memset(plan, 0, sizeof(*plan));
}

static void xaot_bundle_clear_global_lowered_plans(XaotBundle *bundle) {
    uint32_t i;
    if (!bundle)
        return;
    xr_free(bundle->class_hierarchy_plans);
    bundle->class_hierarchy_plans = NULL;
    bundle->nclass_hierarchy_plans = 0;
    bundle->class_hierarchy_plan_cap = 0;

    for (i = 0; i < bundle->nclass_layout_plans; i++)
        xaot_class_layout_plan_free(&bundle->class_layout_plans[i]);
    xr_free(bundle->class_layout_plans);
    bundle->class_layout_plans = NULL;
    bundle->nclass_layout_plans = 0;
    bundle->class_layout_plan_cap = 0;

    xr_free(bundle->method_dispatch_plans);
    bundle->method_dispatch_plans = NULL;
    bundle->nmethod_dispatch_plans = 0;
    bundle->method_dispatch_plan_cap = 0;

    xr_free(bundle->dispatch_target_cases);
    bundle->dispatch_target_cases = NULL;
    bundle->ndispatch_target_cases = 0;
    bundle->dispatch_target_case_cap = 0;

    xr_free(bundle->interface_use_plans);
    bundle->interface_use_plans = NULL;
    bundle->ninterface_use_plans = 0;
    bundle->interface_use_plan_cap = 0;

    xr_free(bundle->interface_abi_plans);
    bundle->interface_abi_plans = NULL;
    bundle->ninterface_abi_plans = 0;
    bundle->interface_abi_plan_cap = 0;

    xr_free(bundle->generic_specialization_plans);
    bundle->generic_specialization_plans = NULL;
    bundle->ngeneric_specialization_plans = 0;
    bundle->generic_specialization_plan_cap = 0;

    xr_free(bundle->generic_instantiation_plans);
    bundle->generic_instantiation_plans = NULL;
    bundle->ngeneric_instantiation_plans = 0;
    bundle->generic_instantiation_plan_cap = 0;

    xr_free(bundle->generic_body_plans);
    bundle->generic_body_plans = NULL;
    bundle->ngeneric_body_plans = 0;
    bundle->generic_body_plan_cap = 0;

    xr_free(bundle->generic_storage_plans);
    bundle->generic_storage_plans = NULL;
    bundle->ngeneric_storage_plans = 0;
    bundle->generic_storage_plan_cap = 0;

    xr_free(bundle->generic_code_size_plans);
    bundle->generic_code_size_plans = NULL;
    bundle->ngeneric_code_size_plans = 0;
    bundle->generic_code_size_plan_cap = 0;

    xr_free(bundle->derive_plans);
    bundle->derive_plans = NULL;
    bundle->nderive_plans = 0;
    bundle->derive_plan_cap = 0;

    xr_free(bundle->derived_eq_hash_plans);
    bundle->derived_eq_hash_plans = NULL;
    bundle->nderived_eq_hash_plans = 0;
    bundle->derived_eq_hash_plan_cap = 0;

    xr_free(bundle->derived_clone_plans);
    bundle->derived_clone_plans = NULL;
    bundle->nderived_clone_plans = 0;
    bundle->derived_clone_plan_cap = 0;

    xr_free(bundle->json_shape_plans);
    bundle->json_shape_plans = NULL;
    bundle->njson_shape_plans = 0;
    bundle->json_shape_plan_cap = 0;

    xr_free(bundle->json_access_plans);
    bundle->json_access_plans = NULL;
    bundle->njson_access_plans = 0;
    bundle->json_access_plan_cap = 0;
    xr_free(bundle->json_codec_plans);
    bundle->json_codec_plans = NULL;
    bundle->njson_codec_plans = 0;
    bundle->json_codec_plan_cap = 0;

    xr_free(bundle->record_shape_plans);
    bundle->record_shape_plans = NULL;
    bundle->nrecord_shape_plans = 0;
    bundle->record_shape_plan_cap = 0;

    xr_free(bundle->record_access_plans);
    bundle->record_access_plans = NULL;
    bundle->nrecord_access_plans = 0;
    bundle->record_access_plan_cap = 0;

    xr_free(bundle->options_plans);
    bundle->options_plans = NULL;
    bundle->noptions_plans = 0;
    bundle->options_plan_cap = 0;

    xr_free(bundle->map_shape_plans);
    bundle->map_shape_plans = NULL;
    bundle->nmap_shape_plans = 0;
    bundle->map_shape_plan_cap = 0;

    xr_free(bundle->key_access_plans);
    bundle->key_access_plans = NULL;
    bundle->nkey_access_plans = 0;
    bundle->key_access_plan_cap = 0;

    xr_free(bundle->hash_eq_plans);
    bundle->hash_eq_plans = NULL;
    bundle->nhash_eq_plans = 0;
    bundle->hash_eq_plan_cap = 0;

    xr_free(bundle->sequence_access_plans);
    bundle->sequence_access_plans = NULL;
    bundle->nsequence_access_plans = 0;
    bundle->sequence_access_plan_cap = 0;

    xr_free(bundle->capacity_plans);
    bundle->capacity_plans = NULL;
    bundle->ncapacity_plans = 0;
    bundle->capacity_plan_cap = 0;

    xr_free(bundle->bulk_plans);
    bundle->bulk_plans = NULL;
    bundle->nbulk_plans = 0;
    bundle->bulk_plan_cap = 0;

    xr_free(bundle->encoding_plans);
    bundle->encoding_plans = NULL;
    bundle->nencoding_plans = 0;
    bundle->encoding_plan_cap = 0;

    xr_free(bundle->metadata_plans);
    bundle->metadata_plans = NULL;
    bundle->nmetadata_plans = 0;
    bundle->metadata_plan_cap = 0;

    xr_free(bundle->capability_plans);
    bundle->capability_plans = NULL;
    bundle->ncapability_plans = 0;
    bundle->capability_plan_cap = 0;

    xr_free(bundle->static_data_plans);
    bundle->static_data_plans = NULL;
    bundle->nstatic_data_plans = 0;
    bundle->static_data_plan_cap = 0;

    xr_free(bundle->link_dependency_plans);
    bundle->link_dependency_plans = NULL;
    bundle->nlink_dependency_plans = 0;
    bundle->link_dependency_plan_cap = 0;
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

static bool xg_class_summary_is_runtime_class(const XgClassSummary *cls) {
    return cls && (cls->decl_kind == 0 || cls->decl_kind == XG_DECL_CLASS);
}

static const XgClassSummary *xg_evidence_find_class(const XgGlobalEvidence *ev,
                                                    XgClassId class_id) {
    if (!ev || class_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < ev->nclasses; i++) {
        if (ev->classes[i].class_id == class_id)
            return &ev->classes[i];
    }
    return NULL;
}

static bool xg_evidence_class_is_descendant_or_self(const XgGlobalEvidence *ev, XgClassId class_id,
                                                    XgClassId ancestor_id) {
    const XgClassSummary *cls;
    uint8_t depth = 0;
    if (!ev || class_id == XG_NO_ID || ancestor_id == XG_NO_ID)
        return false;
    if (class_id == ancestor_id)
        return true;
    cls = xg_evidence_find_class(ev, class_id);
    while (cls && cls->parent_class_id != XG_NO_ID && depth++ < 64) {
        if (cls->parent_class_id == ancestor_id)
            return true;
        cls = xg_evidence_find_class(ev, cls->parent_class_id);
    }
    return false;
}

static const XgMethodSummary *xg_evidence_find_method_in_class(const XgGlobalEvidence *ev,
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

static const XgMethodSummary *xg_evidence_find_method_by_id(const XgGlobalEvidence *ev,
                                                            XgMethodId method_id) {
    if (!ev || method_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < ev->nmethods; i++) {
        if (ev->methods[i].method_id == method_id)
            return &ev->methods[i];
    }
    return NULL;
}

static const XgMethodSummary *
xg_evidence_find_method_by_signature_in_class(const XgGlobalEvidence *ev, const XgClassSummary *cls,
                                              uint32_t name_id, uint32_t signature_key) {
    if (!ev || !cls || cls->method_start == 0 || name_id == 0)
        return NULL;
    for (uint32_t i = 0; i < cls->method_count; i++) {
        uint32_t idx = cls->method_start - 1 + i;
        const XgMethodSummary *method = idx < ev->nmethods ? &ev->methods[idx] : NULL;
        if (method && method->owner_class_id == cls->class_id && method->name_id == name_id &&
            method->signature_key == signature_key && (method->flags & XG_METHOD_STATIC) == 0 &&
            (method->flags & XG_METHOD_CONSTRUCTOR) == 0)
            return method;
    }
    return NULL;
}

static const XgMethodSummary *xg_evidence_find_method_in_hierarchy(const XgGlobalEvidence *ev,
                                                                   XgClassId class_id,
                                                                   XgMethodId method_or_name_id) {
    const XgClassSummary *cls = xg_evidence_find_class(ev, class_id);
    while (cls) {
        const XgMethodSummary *method =
            xg_evidence_find_method_in_class(ev, cls, method_or_name_id);
        if (method)
            return method;
        if (cls->parent_class_id == XG_NO_ID)
            break;
        cls = xg_evidence_find_class(ev, cls->parent_class_id);
    }
    return NULL;
}

static const XgMethodSummary *
xg_evidence_find_method_by_signature_in_hierarchy(const XgGlobalEvidence *ev, XgClassId class_id,
                                                  uint32_t name_id, uint32_t signature_key) {
    const XgClassSummary *cls = xg_evidence_find_class(ev, class_id);
    while (cls) {
        const XgMethodSummary *method =
            xg_evidence_find_method_by_signature_in_class(ev, cls, name_id, signature_key);
        if (method)
            return method;
        if (cls->parent_class_id == XG_NO_ID)
            break;
        cls = xg_evidence_find_class(ev, cls->parent_class_id);
    }
    return NULL;
}

static bool xg_evidence_interface_extends_reaches(const XgGlobalEvidence *ev, XgInterfaceId from,
                                                  XgInterfaceId target, uint32_t depth) {
    if (!ev || from == XG_NO_ID || target == XG_NO_ID || depth > 64)
        return false;
    if (from == target)
        return true;
    for (uint32_t i = 0; i < ev->ninterface_extends; i++) {
        const XgInterfaceExtendsSummary *edge = &ev->interface_extends[i];
        if (edge->child_interface_id != from)
            continue;
        if (xg_evidence_interface_extends_reaches(ev, edge->parent_interface_id, target, depth + 1))
            return true;
    }
    return false;
}

static bool xg_evidence_interface_impl_matches(const XgGlobalEvidence *ev,
                                               XgInterfaceId implementor_interface,
                                               XgInterfaceId receiver_interface) {
    return implementor_interface == receiver_interface ||
           xg_evidence_interface_extends_reaches(ev, implementor_interface, receiver_interface, 0);
}

static bool xg_evidence_effective_interface_implementor_seen(const XgGlobalEvidence *ev,
                                                             XgInterfaceId receiver_interface,
                                                             XgClassId implementor_class,
                                                             uint32_t upto_index) {
    if (!ev || receiver_interface == XG_NO_ID || implementor_class == XG_NO_ID)
        return false;
    for (uint32_t i = 0; i < upto_index && i < ev->ninterface_impls; i++) {
        const XgInterfaceImplSummary *impl = &ev->interface_impls[i];
        if (impl->implementor_class_id == implementor_class &&
            xg_evidence_interface_impl_matches(ev, impl->interface_id, receiver_interface))
            return true;
    }
    return false;
}

static bool xg_evidence_interface_method_visible_from(const XgGlobalEvidence *ev,
                                                      XgInterfaceId receiver_interface_id,
                                                      const XgInterfaceMethodSummary *method) {
    if (!ev || receiver_interface_id == XG_NO_ID || !method)
        return false;
    return xg_evidence_interface_extends_reaches(ev, receiver_interface_id,
                                                 method->owner_interface_id, 0);
}

static uint32_t xg_evidence_interface_dispatch_slot(const XgGlobalEvidence *ev,
                                                    XgInterfaceId receiver_interface_id,
                                                    uint32_t name_id, uint32_t signature_key) {
    uint32_t slot = 0;
    if (!ev || receiver_interface_id == XG_NO_ID || name_id == 0 || signature_key == 0)
        return UINT32_MAX;
    for (uint32_t i = 0; i < ev->ninterface_methods; i++) {
        const XgInterfaceMethodSummary *method = &ev->interface_methods[i];
        if (!xg_evidence_interface_method_visible_from(ev, receiver_interface_id, method))
            continue;
        if (method->name_id == name_id && method->signature_key == signature_key)
            return slot;
        slot++;
    }
    return UINT32_MAX;
}

static uint32_t xg_evidence_interface_visible_method_count(const XgGlobalEvidence *ev,
                                                           XgInterfaceId interface_id) {
    uint32_t count = 0;
    if (!ev || interface_id == XG_NO_ID)
        return 0;
    for (uint32_t i = 0; i < ev->ninterface_methods; i++) {
        if (xg_evidence_interface_method_visible_from(ev, interface_id, &ev->interface_methods[i]))
            count++;
    }
    return count;
}

static uint32_t xg_evidence_effective_interface_implementor_count(const XgGlobalEvidence *ev,
                                                                  XgInterfaceId interface_id) {
    uint32_t count = 0;
    if (!ev || interface_id == XG_NO_ID)
        return 0;
    for (uint32_t i = 0; i < ev->ninterface_impls; i++) {
        const XgInterfaceImplSummary *impl = &ev->interface_impls[i];
        if (!xg_evidence_interface_impl_matches(ev, impl->interface_id, interface_id))
            continue;
        if (xg_evidence_effective_interface_implementor_seen(ev, interface_id,
                                                             impl->implementor_class_id, i))
            continue;
        count++;
    }
    return count;
}

static bool xaot_bundle_add_interface_call_use_plan(XaotBundle *bundle,
                                                    const XgCallsiteSummary *call,
                                                    XgClassId implementor_class_id, uint8_t kind);

static XgFuncId xaot_bundle_find_method_body_func_id(const XgGlobalEvidence *evidence,
                                                     XgMethodId method_id) {
    XgFuncId match = XG_NO_ID;
    if (!evidence || method_id == XG_NO_ID)
        return XG_NO_ID;
    for (uint32_t i = 0; i < evidence->nbodies; i++) {
        const XgBodySummary *body = &evidence->bodies[i];
        if (body->kind != XG_BODY_METHOD || body->owner_method_id != method_id)
            continue;
        if (match != XG_NO_ID && match != body->func_id)
            return XG_NO_ID;
        match = body->func_id;
    }
    return match;
}

static bool xaot_bundle_add_dispatch_target_case(XaotBundle *bundle, XgCallsiteId callsite_id,
                                                 XgClassId receiver_class_id,
                                                 const XgGlobalEvidence *evidence,
                                                 const XgMethodSummary *method,
                                                 uint32_t evidence_bits) {
    XaotDispatchTargetCase *target;
    if (!bundle || callsite_id == XG_NO_ID || receiver_class_id == XG_NO_ID || !method ||
        method->method_id == XG_NO_ID)
        return false;
    if (!reserve_plan_array((void **) &bundle->dispatch_target_cases,
                            &bundle->dispatch_target_case_cap, bundle->ndispatch_target_cases + 1,
                            sizeof(XaotDispatchTargetCase), 8))
        return false;
    target = &bundle->dispatch_target_cases[bundle->ndispatch_target_cases++];
    memset(target, 0, sizeof(*target));
    target->callsite_id = callsite_id;
    target->receiver_class_id = receiver_class_id;
    target->method_id = method->method_id;
    target->method_owner_class_id = method->owner_class_id;
    target->method_body_func_id = xaot_bundle_find_method_body_func_id(evidence, method->method_id);
    target->method_name_id = method->name_id;
    target->method_signature_key = method->signature_key;
    target->method_root_id = method->root_method_id;
    target->method_override_depth = method->override_depth;
    target->evidence = evidence_bits;
    return true;
}

static bool xaot_bundle_add_class_hierarchy_plan(XaotBundle *bundle,
                                                 const XgClassSummary *summary) {
    XaotClassHierarchyPlan *plan;
    if (!bundle || !summary)
        return false;
    if (!reserve_plan_array((void **) &bundle->class_hierarchy_plans,
                            &bundle->class_hierarchy_plan_cap, bundle->nclass_hierarchy_plans + 1,
                            sizeof(XaotClassHierarchyPlan), 8))
        return false;
    plan = &bundle->class_hierarchy_plans[bundle->nclass_hierarchy_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->class_id = summary->class_id;
    plan->parent_class_id = summary->parent_class_id;
    plan->flags =
        summary->flags & (XG_CLASS_EXPLICIT_FINAL | XG_CLASS_HAS_SUBCLASS |
                          XG_CLASS_INFERRED_FINAL | XG_CLASS_NATIVE | XG_CLASS_RUNTIME_ONLY |
                          XG_CLASS_GENERIC_SKELETON | XG_CLASS_MONOMORPHIZED);
    plan->evidence = XAOT_CLASS_HIER_EV_GLOBAL_SUMMARY | XAOT_CLASS_HIER_EV_FINALITY_DERIVED;
    if (summary->parent_class_id != XG_NO_ID)
        plan->evidence |= XAOT_CLASS_HIER_EV_PARENT_RESOLVED;
    return true;
}

static bool xaot_bundle_add_class_layout_plan(XaotBundle *bundle, const XgClassSummary *summary) {
    XaotClassLayoutPlan *plan;
    char ctype[64];
    uint32_t flags = XAOT_CLASS_LAYOUT_HEADER | XAOT_CLASS_LAYOUT_TYPED_PAYLOAD;
    if (!bundle || !summary)
        return false;
    if (!reserve_plan_array((void **) &bundle->class_layout_plans, &bundle->class_layout_plan_cap,
                            bundle->nclass_layout_plans + 1, sizeof(XaotClassLayoutPlan), 8))
        return false;
    if (summary->parent_class_id != XG_NO_ID)
        flags |= XAOT_CLASS_LAYOUT_PREFIX_PARENT | XAOT_CLASS_LAYOUT_TYPE_ID;
    if ((summary->flags & XG_CLASS_HAS_SUBCLASS) != 0)
        flags |= XAOT_CLASS_LAYOUT_TYPE_ID;
    plan = &bundle->class_layout_plans[bundle->nclass_layout_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->class_id = summary->class_id;
    snprintf(ctype, sizeof(ctype), "XrC_%u", summary->class_id);
    plan->c_type_name = xr_strdup(ctype);
    if (!plan->c_type_name) {
        bundle->nclass_layout_plans--;
        return false;
    }
    plan->field_start = summary->field_start;
    plan->field_count = summary->field_count;
    plan->flags = flags;
    return true;
}

static bool xaot_bundle_add_interface_call_use_plan(XaotBundle *bundle,
                                                    const XgCallsiteSummary *call,
                                                    XgClassId implementor_class_id, uint8_t kind);

static bool xaot_bundle_add_method_dispatch_plan(XaotBundle *bundle, const XgCallsiteSummary *call,
                                                 const XgGlobalEvidence *ev) {
    enum {
        XAOT_DISPATCH_SMALL_IMPLEMENTOR_LIMIT = 4
    };
    XaotMethodDispatchPlan *plan;
    const XgClassSummary *receiver_cls = NULL;
    const XgMethodSummary *method = NULL;
    uint8_t kind = XAOT_DISPATCH_RUNTIME_FALLBACK;
    uint32_t evidence = XAOT_DISPATCH_EV_GLOBAL_CALLSITE;
    uint8_t reason = XAOT_DISPATCH_UNPROVEN_NONE;
    uint32_t target_start = 0;
    uint16_t target_count = 0;
    uint32_t dispatch_slot = UINT32_MAX;

    if (!bundle || !call || !ev)
        return false;
    if (!reserve_plan_array((void **) &bundle->method_dispatch_plans,
                            &bundle->method_dispatch_plan_cap, bundle->nmethod_dispatch_plans + 1,
                            sizeof(XaotMethodDispatchPlan), 8))
        return false;

    if (call->kind == XG_CALL_INTERFACE) {
        uint32_t implementor_count = 0;
        bool all_targets_resolved = true;
        evidence |= XAOT_DISPATCH_EV_INTERFACE_OBJECT;
        if (call->receiver_static_interface_id == XG_NO_ID) {
            kind = XAOT_DISPATCH_ITABLE;
            reason = XAOT_DISPATCH_UNPROVEN_NO_INTERFACE_ID;
        } else {
            dispatch_slot = xg_evidence_interface_dispatch_slot(
                ev, call->receiver_static_interface_id, call->method_name_id,
                call->method_signature_key);
            for (uint32_t i = 0; i < ev->ninterface_impls; i++) {
                const XgInterfaceImplSummary *impl = &ev->interface_impls[i];
                const XgMethodSummary *target_method;
                if (!xg_evidence_interface_impl_matches(ev, impl->interface_id,
                                                        call->receiver_static_interface_id))
                    continue;
                if (xg_evidence_effective_interface_implementor_seen(
                        ev, call->receiver_static_interface_id, impl->implementor_class_id, i))
                    continue;
                implementor_count++;
                target_method = xg_evidence_find_method_by_signature_in_hierarchy(
                    ev, impl->implementor_class_id, call->method_name_id,
                    call->method_signature_key);
                if (!target_method)
                    all_targets_resolved = false;
            }
            if (implementor_count == 0 || !all_targets_resolved) {
                kind = XAOT_DISPATCH_ITABLE;
                reason = XAOT_DISPATCH_UNPROVEN_NO_TARGET_METHOD;
            } else if (implementor_count == 1) {
                kind = XAOT_DISPATCH_DIRECT;
                evidence |= XAOT_DISPATCH_EV_SINGLE_IMPLEMENTOR;
            } else if (implementor_count <= XAOT_DISPATCH_SMALL_IMPLEMENTOR_LIMIT) {
                kind = XAOT_DISPATCH_TYPE_SWITCH;
                evidence |= XAOT_DISPATCH_EV_SMALL_IMPLEMENTOR_SET;
            } else {
                kind = XAOT_DISPATCH_ITABLE;
                reason = XAOT_DISPATCH_UNPROVEN_LARGE_IMPLEMENTOR_SET;
            }
        }
    } else if (call->method_id == XG_NO_ID) {
        reason = XAOT_DISPATCH_UNPROVEN_NO_METHOD_ID;
    } else if (call->receiver_static_class_id == XG_NO_ID) {
        reason = XAOT_DISPATCH_UNPROVEN_NO_RECEIVER_TYPE;
    } else {
        receiver_cls = xg_evidence_find_class(ev, call->receiver_static_class_id);
        method = xg_evidence_find_method_in_hierarchy(ev, call->receiver_static_class_id,
                                                      call->method_id);
        if (!method) {
            reason = XAOT_DISPATCH_UNPROVEN_NO_METHOD_ID;
        } else if (receiver_cls && (receiver_cls->flags &
                                    (XG_CLASS_EXPLICIT_FINAL | XG_CLASS_INFERRED_FINAL)) != 0) {
            kind = XAOT_DISPATCH_DIRECT;
            evidence |= XAOT_DISPATCH_EV_RECEIVER_CONCRETE | XAOT_DISPATCH_EV_INFERRED_FINAL;
        } else if ((method->flags & XG_METHOD_OVERRIDDEN) == 0) {
            kind = XAOT_DISPATCH_DIRECT;
            evidence |= XAOT_DISPATCH_EV_METHOD_NOT_OVERRIDDEN;
        } else {
            kind = XAOT_DISPATCH_VTABLE;
            reason = XAOT_DISPATCH_UNPROVEN_POLYMORPHIC;
            evidence |= XAOT_DISPATCH_EV_OVERRIDE_GRAPH;
        }
    }

    if (kind == XAOT_DISPATCH_DIRECT && method) {
        target_start = bundle->ndispatch_target_cases + 1;
        if (!xaot_bundle_add_dispatch_target_case(
                bundle, call->callsite_id, call->receiver_static_class_id, ev, method, evidence))
            return false;
        target_count = 1;
    } else if ((kind == XAOT_DISPATCH_DIRECT || kind == XAOT_DISPATCH_TYPE_SWITCH ||
                (kind == XAOT_DISPATCH_ITABLE &&
                 reason == XAOT_DISPATCH_UNPROVEN_LARGE_IMPLEMENTOR_SET)) &&
               call->kind == XG_CALL_INTERFACE) {
        target_start = bundle->ndispatch_target_cases + 1;
        for (uint32_t i = 0; i < ev->ninterface_impls; i++) {
            const XgInterfaceImplSummary *impl = &ev->interface_impls[i];
            const XgMethodSummary *target_method;
            if (!xg_evidence_interface_impl_matches(ev, impl->interface_id,
                                                    call->receiver_static_interface_id))
                continue;
            if (xg_evidence_effective_interface_implementor_seen(
                    ev, call->receiver_static_interface_id, impl->implementor_class_id, i))
                continue;
            target_method = xg_evidence_find_method_by_signature_in_hierarchy(
                ev, impl->implementor_class_id, call->method_name_id, call->method_signature_key);
            if (!target_method ||
                !xaot_bundle_add_dispatch_target_case(bundle, call->callsite_id,
                                                      impl->implementor_class_id, ev, target_method,
                                                      evidence) ||
                !xaot_bundle_add_interface_call_use_plan(bundle, call, impl->implementor_class_id,
                                                         kind))
                return false;
            target_count++;
        }
    } else if (kind == XAOT_DISPATCH_VTABLE && call->receiver_static_class_id != XG_NO_ID) {
        target_start = bundle->ndispatch_target_cases + 1;
        for (uint32_t i = 0; i < ev->nclasses; i++) {
            const XgClassSummary *candidate = &ev->classes[i];
            const XgMethodSummary *target_method;
            if (!xg_class_summary_is_runtime_class(candidate))
                continue;
            if (!xg_evidence_class_is_descendant_or_self(ev, candidate->class_id,
                                                         call->receiver_static_class_id))
                continue;
            target_method = xg_evidence_find_method_by_signature_in_hierarchy(
                ev, candidate->class_id, call->method_name_id, call->method_signature_key);
            if (!target_method || !xaot_bundle_add_dispatch_target_case(bundle, call->callsite_id,
                                                                        candidate->class_id, ev,
                                                                        target_method, evidence))
                return false;
            target_count++;
        }
        if (target_count == 0)
            return false;
    }

    plan = &bundle->method_dispatch_plans[bundle->nmethod_dispatch_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->callsite_id = call->callsite_id;
    plan->owner_func_id = call->owner_func_id;
    plan->source_span_id = call->source_span_id;
    plan->body_ordinal = call->body_ordinal;
    plan->method_id = method ? method->method_id : call->method_id;
    plan->method_root_id = method ? method->root_method_id : XG_NO_ID;
    plan->method_name_id = call->method_name_id;
    plan->method_signature_key = call->method_signature_key;
    plan->arg_type_key_start = call->arg_type_key_start;
    plan->arg_count = call->arg_count;
    plan->receiver_static_class_id = call->receiver_static_class_id;
    plan->receiver_static_interface_id = call->receiver_static_interface_id;
    plan->kind = kind;
    plan->dispatch_slot = dispatch_slot;
    plan->target_start = target_start;
    plan->target_count = target_count;
    plan->evidence = kind == XAOT_DISPATCH_RUNTIME_FALLBACK ? 0 : evidence;
    plan->unproven_reason = reason;
    return true;
}

static bool xaot_bundle_add_interface_use_plan_row(XaotBundle *bundle, XgInterfaceId interface_id,
                                                   XgClassId implementor_class_id,
                                                   XgCallsiteId use_site_id, uint32_t reason,
                                                   uint32_t flags) {
    XaotInterfaceUsePlan *plan;
    if (!bundle || interface_id == XG_NO_ID || implementor_class_id == XG_NO_ID || reason == 0)
        return false;
    for (uint32_t i = 0; i < bundle->ninterface_use_plans; i++) {
        plan = &bundle->interface_use_plans[i];
        if (plan->interface_id == interface_id &&
            plan->implementor_class_id == implementor_class_id &&
            plan->use_site_id == use_site_id) {
            plan->reason |= reason;
            plan->flags |= flags;
            return true;
        }
    }
    if (!reserve_plan_array((void **) &bundle->interface_use_plans, &bundle->interface_use_plan_cap,
                            bundle->ninterface_use_plans + 1, sizeof(XaotInterfaceUsePlan), 8))
        return false;
    plan = &bundle->interface_use_plans[bundle->ninterface_use_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->interface_id = interface_id;
    plan->implementor_class_id = implementor_class_id;
    plan->use_site_id = use_site_id;
    plan->reason = reason;
    plan->flags = flags;
    return true;
}

static bool xaot_bundle_add_interface_use_plan(XaotBundle *bundle,
                                               const XgInterfaceImplSummary *impl) {
    if (!bundle || !impl)
        return false;
    return xaot_bundle_add_interface_use_plan_row(bundle, impl->interface_id,
                                                  impl->implementor_class_id, XG_NO_ID,
                                                  XAOT_INTERFACE_USE_REASON_IMPLEMENTS, 0);
}

static bool xaot_bundle_add_interface_call_use_plan(XaotBundle *bundle,
                                                    const XgCallsiteSummary *call,
                                                    XgClassId implementor_class_id, uint8_t kind) {
    uint32_t flags = XAOT_INTERFACE_USE_NEEDS_IFACE_OBJECT;
    if (kind == XAOT_DISPATCH_TYPE_SWITCH)
        flags |= XAOT_INTERFACE_USE_TYPE_SWITCHABLE;
    else if (kind == XAOT_DISPATCH_ITABLE)
        flags |= XAOT_INTERFACE_USE_NEEDS_ITABLE;
    if (!bundle || !call || call->kind != XG_CALL_INTERFACE)
        return false;
    return xaot_bundle_add_interface_use_plan_row(bundle, call->receiver_static_interface_id,
                                                  implementor_class_id, call->callsite_id,
                                                  XAOT_INTERFACE_USE_REASON_VALUE, flags);
}

static uint32_t xaot_interface_use_reason_from_object_use(uint32_t reason) {
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

static bool xaot_bundle_add_interface_object_use_plan(XaotBundle *bundle,
                                                      const XgGlobalEvidence *ev,
                                                      const XgInterfaceObjectUseSummary *use) {
    uint32_t reason;
    uint32_t flags = XAOT_INTERFACE_USE_NEEDS_IFACE_OBJECT;
    if (!bundle || !ev || !use || use->interface_id == XG_NO_ID)
        return false;
    reason = xaot_interface_use_reason_from_object_use(use->reason);
    if (reason == 0)
        return false;
    if (xg_evidence_interface_visible_method_count(ev, use->interface_id) != 0)
        flags |= XAOT_INTERFACE_USE_NEEDS_ITABLE;
    for (uint32_t i = 0; i < ev->ninterface_impls; i++) {
        const XgInterfaceImplSummary *impl = &ev->interface_impls[i];
        if (!xg_evidence_interface_impl_matches(ev, impl->interface_id, use->interface_id))
            continue;
        if (xg_evidence_effective_interface_implementor_seen(ev, use->interface_id,
                                                             impl->implementor_class_id, i))
            continue;
        if (!xaot_bundle_add_interface_use_plan_row(
                bundle, use->interface_id, impl->implementor_class_id, XG_NO_ID, reason, flags))
            return false;
    }
    return true;
}

static bool xaot_bundle_add_interface_abi_plan(XaotBundle *bundle, const XgGlobalEvidence *ev,
                                               XgInterfaceId interface_id) {
    XaotInterfaceAbiPlan *plan;
    uint32_t callsite_count = 0;
    uint32_t object_use_count = 0;
    uint32_t flags = 0;
    uint32_t evidence = 0;
    uint8_t data_source = XAOT_INTERFACE_ABI_SOURCE_NONE;
    uint8_t type_source = XAOT_INTERFACE_ABI_SOURCE_NONE;
    uint8_t itable_source = XAOT_INTERFACE_ABI_SOURCE_NONE;
    uint8_t tag_source = XAOT_INTERFACE_ABI_SOURCE_NONE;

    if (!bundle || !ev || interface_id == XG_NO_ID)
        return false;
    for (uint32_t i = 0; i < bundle->ninterface_abi_plans; i++) {
        if (bundle->interface_abi_plans[i].interface_id == interface_id)
            return true;
    }
    for (uint32_t i = 0; i < ev->ncallsites; i++) {
        const XgCallsiteSummary *call = &ev->callsites[i];
        const XaotMethodDispatchPlan *dispatch;
        if (call->kind != XG_CALL_INTERFACE || call->receiver_static_interface_id != interface_id)
            continue;
        dispatch = xaot_bundle_find_method_dispatch_plan(bundle, call->callsite_id);
        if (!dispatch)
            return false;
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
        if (use->interface_id != interface_id)
            continue;
        object_use_count++;
        flags |= XAOT_INTERFACE_ABI_NEEDS_IFACE_OBJECT | XAOT_INTERFACE_ABI_BOXED_RECEIVER;
        data_source = XAOT_INTERFACE_ABI_SOURCE_BOXED_VALUE;
        type_source = XAOT_INTERFACE_ABI_SOURCE_NATIVE_TYPE_ID;
        evidence |= XAOT_INTERFACE_ABI_EV_OBJECT_USE;
    }
    if (callsite_count == 0 && object_use_count == 0)
        return true;
    if (xg_evidence_interface_visible_method_count(ev, interface_id) != 0)
        evidence |= XAOT_INTERFACE_ABI_EV_INTERFACE_METHODS;
    if (xg_evidence_effective_interface_implementor_count(ev, interface_id) != 0)
        evidence |= XAOT_INTERFACE_ABI_EV_IMPLEMENTOR_SET;
    if (object_use_count != 0 &&
        xg_evidence_interface_visible_method_count(ev, interface_id) != 0) {
        flags |= XAOT_INTERFACE_ABI_NEEDS_ITABLE;
        itable_source = XAOT_INTERFACE_ABI_SOURCE_DISPATCH_SLOT;
    }
    if (!reserve_plan_array((void **) &bundle->interface_abi_plans, &bundle->interface_abi_plan_cap,
                            bundle->ninterface_abi_plans + 1, sizeof(XaotInterfaceAbiPlan), 8))
        return false;
    plan = &bundle->interface_abi_plans[bundle->ninterface_abi_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->interface_id = interface_id;
    plan->callsite_count = callsite_count;
    plan->implementor_count = xg_evidence_effective_interface_implementor_count(ev, interface_id);
    plan->method_slot_count = xg_evidence_interface_visible_method_count(ev, interface_id);
    plan->flags = flags;
    plan->data_source = data_source;
    plan->type_source = type_source;
    plan->itable_source = itable_source;
    plan->tag_source = tag_source;
    plan->evidence = evidence;
    plan->unproven_reason = XAOT_INTERFACE_ABI_UNPROVEN_NONE;
    return true;
}

static bool xaot_bundle_add_interface_abi_plans(XaotBundle *bundle,
                                                const XgGlobalEvidence *evidence) {
    if (!bundle || !evidence)
        return false;
    for (uint32_t i = 0; i < evidence->ncallsites; i++) {
        const XgCallsiteSummary *call = &evidence->callsites[i];
        if (call->kind != XG_CALL_INTERFACE || call->receiver_static_interface_id == XG_NO_ID)
            continue;
        if (!xaot_bundle_add_interface_abi_plan(bundle, evidence,
                                                call->receiver_static_interface_id))
            return false;
    }
    for (uint32_t i = 0; i < evidence->ninterface_object_uses; i++) {
        const XgInterfaceObjectUseSummary *use = &evidence->interface_object_uses[i];
        if (!xaot_bundle_add_interface_abi_plan(bundle, evidence, use->interface_id))
            return false;
    }
    return true;
}

static uint8_t specialization_action_for_dispatch(uint8_t dispatch_kind) {
    switch ((XaotMethodDispatchKind) dispatch_kind) {
        case XAOT_DISPATCH_DIRECT:
            return XAOT_SPECIALIZATION_DIRECT;
        case XAOT_DISPATCH_TYPE_SWITCH:
            return XAOT_SPECIALIZATION_TYPE_SWITCH;
        default:
            return XAOT_SPECIALIZATION_FALLBACK;
    }
}

static uint8_t specialization_reason_for_dispatch(const XaotMethodDispatchPlan *dispatch) {
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

static XgClassId specialization_single_implementor_from_dispatch(const XaotBundle *bundle,
                                                                 const XaotMethodDispatchPlan *dp) {
    if (!bundle || !dp || dp->target_count != 1 || dp->target_start == 0)
        return XG_NO_ID;
    if (dp->target_start - 1 >= bundle->ndispatch_target_cases)
        return XG_NO_ID;
    return bundle->dispatch_target_cases[dp->target_start - 1].receiver_class_id;
}

static bool xaot_bundle_add_generic_specialization_plan(XaotBundle *bundle,
                                                        const XgGlobalEvidence *evidence,
                                                        const XgCallsiteSummary *call) {
    XaotGenericSpecializationPlan *plan;
    const XaotMethodDispatchPlan *dispatch;
    uint32_t evidence_bits = XAOT_SPECIALIZATION_EV_GLOBAL_CALLSITE;
    if (!bundle || !evidence || !call || call->kind != XG_CALL_INTERFACE)
        return false;
    dispatch = xaot_bundle_find_method_dispatch_plan(bundle, call->callsite_id);
    if (dispatch)
        evidence_bits |= XAOT_SPECIALIZATION_EV_DISPATCH_PLAN;
    if (call->receiver_static_interface_id != XG_NO_ID &&
        xg_evidence_effective_interface_implementor_count(evidence,
                                                          call->receiver_static_interface_id) != 0)
        evidence_bits |= XAOT_SPECIALIZATION_EV_IMPLEMENTOR_SET;
    if (dispatch && dispatch->target_count != 0)
        evidence_bits |= XAOT_SPECIALIZATION_EV_TARGET_CASES;
    if (!reserve_plan_array((void **) &bundle->generic_specialization_plans,
                            &bundle->generic_specialization_plan_cap,
                            bundle->ngeneric_specialization_plans + 1,
                            sizeof(XaotGenericSpecializationPlan), 8))
        return false;
    plan = &bundle->generic_specialization_plans[bundle->ngeneric_specialization_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->callsite_id = call->callsite_id;
    plan->owner_func_id = call->owner_func_id;
    plan->interface_id = call->receiver_static_interface_id;
    plan->method_name_id = call->method_name_id;
    plan->method_signature_key = call->method_signature_key;
    plan->single_implementor_class_id =
        specialization_single_implementor_from_dispatch(bundle, dispatch);
    plan->implementor_count = xg_evidence_effective_interface_implementor_count(
        evidence, call->receiver_static_interface_id);
    plan->target_count = dispatch ? dispatch->target_count : 0;
    plan->dispatch_kind = dispatch ? dispatch->kind : XAOT_DISPATCH_RUNTIME_FALLBACK;
    plan->action = specialization_action_for_dispatch(plan->dispatch_kind);
    plan->evidence = evidence_bits;
    plan->unproven_reason = specialization_reason_for_dispatch(dispatch);
    return true;
}

static bool xaot_bundle_add_generic_specialization_plans(XaotBundle *bundle,
                                                         const XgGlobalEvidence *evidence) {
    if (!bundle || !evidence)
        return false;
    for (uint32_t i = 0; i < evidence->ncallsites; i++) {
        const XgCallsiteSummary *call = &evidence->callsites[i];
        if (call->kind != XG_CALL_INTERFACE)
            continue;
        if (!xaot_bundle_add_generic_specialization_plan(bundle, evidence, call))
            return false;
    }
    return true;
}

static uint8_t generic_instantiation_action_for(const XgGenericInstSummary *inst) {
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

static uint8_t generic_instantiation_reason_for(const XgGenericInstSummary *inst) {
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

static uint32_t generic_instantiation_evidence_for(const XgGenericInstSummary *inst) {
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

static bool xaot_bundle_add_generic_instantiation_plan(XaotBundle *bundle,
                                                       const XgGenericInstSummary *inst) {
    XaotGenericInstantiationPlan *plan;
    if (!bundle || !inst)
        return false;
    if (!reserve_plan_array(
            (void **) &bundle->generic_instantiation_plans, &bundle->generic_instantiation_plan_cap,
            bundle->ngeneric_instantiation_plans + 1, sizeof(XaotGenericInstantiationPlan), 8))
        return false;
    plan = &bundle->generic_instantiation_plans[bundle->ngeneric_instantiation_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->generic_inst_id = inst->generic_inst_id;
    plan->module_id = inst->module_id;
    plan->origin_decl_id = inst->origin_decl_id;
    plan->origin_func_id = inst->origin_func_id;
    plan->origin_method_id = inst->origin_method_id;
    plan->origin_class_id = inst->origin_class_id;
    plan->specialized_func_id = inst->specialized_func_id;
    plan->specialized_class_id = inst->specialized_class_id;
    plan->root_callsite_id = inst->root_callsite_id;
    plan->constraint_interface_id = inst->constraint_interface_id;
    plan->name_id = inst->name_id;
    plan->type_key = inst->type_key;
    plan->type_arg_key_start = inst->type_arg_key_start;
    plan->type_arg_count = inst->type_arg_count;
    plan->inst_kind = inst->kind;
    plan->action = generic_instantiation_action_for(inst);
    plan->evidence = generic_instantiation_evidence_for(inst);
    plan->unproven_reason = generic_instantiation_reason_for(inst);
    return true;
}

static bool xaot_bundle_add_generic_instantiation_plans(XaotBundle *bundle,
                                                        const XgGlobalEvidence *evidence) {
    if (!bundle || !evidence)
        return false;
    for (uint32_t i = 0; i < evidence->ngeneric_insts; i++) {
        if (!xaot_bundle_add_generic_instantiation_plan(bundle, &evidence->generic_insts[i]))
            return false;
    }
    return true;
}

static uint32_t generic_deepen_inst_evidence(const XgGlobalEvidence *evidence,
                                             XgGenericInstId generic_inst_id) {
    const XgGenericInstSummary *inst =
        xg_global_evidence_find_generic_inst(evidence, generic_inst_id);
    if (!inst)
        return 0;
    return XAOT_GENERIC_BODY_EV_GENERIC_INST | XAOT_GENERIC_STORAGE_EV_GENERIC_INST |
           XAOT_GENERIC_CODESIZE_EV_GENERIC_INST;
}

static uint8_t generic_code_size_action_for(const XgGenericCodeSizeSummary *size);

static const XgGenericCodeSizeSummary *
generic_code_size_for_body_use(const XgGlobalEvidence *evidence, XgGenericBodyUseId use_id) {
    if (!evidence || use_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < evidence->ngeneric_code_sizes; i++) {
        const XgGenericCodeSizeSummary *size = &evidence->generic_code_sizes[i];
        if (size->body_use_id == use_id)
            return size;
    }
    return NULL;
}

static bool generic_body_uses_code_size_share_policy(const XgGlobalEvidence *evidence,
                                                     const XgGenericBodyUseSummary *use) {
    const XgGenericCodeSizeSummary *size =
        use ? generic_code_size_for_body_use(evidence, use->use_id) : NULL;
    return size && generic_code_size_action_for(size) == XAOT_GENERIC_CODESIZE_SHARE_CANONICAL_BODY;
}

static uint8_t generic_body_action_for(const XgGlobalEvidence *evidence,
                                       const XgGenericBodyUseSummary *use) {
    const XgGenericInstSummary *inst =
        use ? xg_global_evidence_find_generic_inst(evidence, use->generic_inst_id) : NULL;
    if (!use || !inst || (inst->flags & XG_GENERIC_INST_CONCRETE_TYPES) == 0)
        return XAOT_GENERIC_BODY_REJECT;
    if ((use->flags & XG_GENERIC_BODY_DYNAMIC_BOUNDARY) != 0)
        return XAOT_GENERIC_BODY_REJECT;
    if (generic_body_uses_code_size_share_policy(evidence, use))
        return XAOT_GENERIC_BODY_SHARE_CANONICAL_BODY;
    if (use->specialized_body_func_id != XG_NO_ID ||
        (inst->flags & XG_GENERIC_INST_SPECIALIZED_BODY) != 0)
        return XAOT_GENERIC_BODY_CLONE;
    if ((inst->flags & XG_GENERIC_INST_INTERFACE_CONSTRAINT) != 0)
        return XAOT_GENERIC_BODY_DIRECT_CONSTRAINT_CALL;
    return XAOT_GENERIC_BODY_SHARE_CANONICAL_BODY;
}

static uint8_t generic_body_reason_for(const XgGlobalEvidence *evidence,
                                       const XgGenericBodyUseSummary *use) {
    const XgGenericInstSummary *inst =
        use ? xg_global_evidence_find_generic_inst(evidence, use->generic_inst_id) : NULL;
    if (!use || !inst || (inst->flags & XG_GENERIC_INST_CONCRETE_TYPES) == 0)
        return XAOT_GENERIC_DEEPEN_UNPROVEN_MISSING_CONCRETE_TYPES;
    if ((use->flags & XG_GENERIC_BODY_DYNAMIC_BOUNDARY) != 0)
        return XAOT_GENERIC_DEEPEN_UNPROVEN_DYNAMIC_BOUNDARY;
    if (generic_body_uses_code_size_share_policy(evidence, use))
        return XAOT_GENERIC_DEEPEN_UNPROVEN_CODESIZE_THRESHOLD;
    if (generic_body_action_for(evidence, use) == XAOT_GENERIC_BODY_SHARE_CANONICAL_BODY)
        return XAOT_GENERIC_DEEPEN_UNPROVEN_NO_SPECIALIZED_BODY;
    return XAOT_GENERIC_DEEPEN_UNPROVEN_NONE;
}

static uint32_t generic_body_evidence_for(const XgGlobalEvidence *evidence,
                                          const XgGenericBodyUseSummary *use) {
    uint32_t bits = XAOT_GENERIC_BODY_EV_GLOBAL_ROW;
    if (!use)
        return bits;
    bits |= generic_deepen_inst_evidence(evidence, use->generic_inst_id) &
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

static bool xaot_bundle_add_generic_body_plan(XaotBundle *bundle, const XgGlobalEvidence *evidence,
                                              const XgGenericBodyUseSummary *use) {
    XaotGenericBodyPlan *plan;
    if (!bundle || !evidence || !use)
        return false;
    if (!reserve_plan_array((void **) &bundle->generic_body_plans, &bundle->generic_body_plan_cap,
                            bundle->ngeneric_body_plans + 1, sizeof(XaotGenericBodyPlan), 8))
        return false;
    plan = &bundle->generic_body_plans[bundle->ngeneric_body_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->use_id = use->use_id;
    plan->generic_inst_id = use->generic_inst_id;
    plan->module_id = use->module_id;
    plan->owner_func_id = use->owner_func_id;
    plan->origin_body_func_id = use->origin_body_func_id;
    plan->specialized_body_func_id = use->specialized_body_func_id;
    plan->root_callsite_id = use->root_callsite_id;
    plan->type_key = use->type_key;
    plan->type_arg_key_start = use->type_arg_key_start;
    plan->type_arg_count = use->type_arg_count;
    plan->estimated_body_size = use->estimated_body_size;
    plan->action = generic_body_action_for(evidence, use);
    plan->evidence = generic_body_evidence_for(evidence, use);
    plan->unproven_reason = generic_body_reason_for(evidence, use);
    return true;
}

static bool xaot_bundle_add_generic_body_plans(XaotBundle *bundle,
                                               const XgGlobalEvidence *evidence) {
    if (!bundle || !evidence)
        return false;
    for (uint32_t i = 0; i < evidence->ngeneric_body_uses; i++) {
        if (!xaot_bundle_add_generic_body_plan(bundle, evidence, &evidence->generic_body_uses[i]))
            return false;
    }
    return true;
}

static uint8_t generic_storage_action_for(const XgGenericStorageSummary *storage) {
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

static uint8_t generic_storage_reason_for(const XgGlobalEvidence *evidence,
                                          const XgGenericStorageSummary *storage) {
    const XgGenericInstSummary *inst =
        storage ? xg_global_evidence_find_generic_inst(evidence, storage->generic_inst_id) : NULL;
    if (!storage || !inst || (inst->flags & XG_GENERIC_INST_CONCRETE_TYPES) == 0)
        return XAOT_GENERIC_DEEPEN_UNPROVEN_MISSING_CONCRETE_TYPES;
    if (generic_storage_action_for(storage) == XAOT_GENERIC_STORAGE_REJECT)
        return XAOT_GENERIC_DEEPEN_UNPROVEN_UNSUPPORTED_STORAGE;
    return XAOT_GENERIC_DEEPEN_UNPROVEN_NONE;
}

static uint32_t generic_storage_evidence_for(const XgGlobalEvidence *evidence,
                                             const XgGenericStorageSummary *storage) {
    uint32_t bits = XAOT_GENERIC_STORAGE_EV_GLOBAL_ROW;
    if (!storage)
        return bits;
    bits |= generic_deepen_inst_evidence(evidence, storage->generic_inst_id) &
            XAOT_GENERIC_STORAGE_EV_GENERIC_INST;
    if (storage->specialized_type_key != 0)
        bits |= XAOT_GENERIC_STORAGE_EV_SPECIALIZED_TYPE;
    if (storage->container_plan_id != 0)
        bits |= XAOT_GENERIC_STORAGE_EV_CONTAINER_PLAN;
    return bits;
}

static bool xaot_bundle_add_generic_storage_plan(XaotBundle *bundle,
                                                 const XgGlobalEvidence *evidence,
                                                 const XgGenericStorageSummary *storage) {
    XaotGenericStoragePlan *plan;
    if (!bundle || !evidence || !storage)
        return false;
    if (!reserve_plan_array((void **) &bundle->generic_storage_plans,
                            &bundle->generic_storage_plan_cap, bundle->ngeneric_storage_plans + 1,
                            sizeof(XaotGenericStoragePlan), 8))
        return false;
    plan = &bundle->generic_storage_plans[bundle->ngeneric_storage_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->storage_id = storage->storage_id;
    plan->generic_inst_id = storage->generic_inst_id;
    plan->module_id = storage->module_id;
    plan->storage_kind = storage->storage_kind;
    plan->action = generic_storage_action_for(storage);
    plan->origin_type_key = storage->origin_type_key;
    plan->specialized_type_key = storage->specialized_type_key;
    plan->elem_type_key = storage->elem_type_key;
    plan->key_type_key = storage->key_type_key;
    plan->value_type_key = storage->value_type_key;
    plan->container_plan_id = storage->container_plan_id;
    plan->evidence = generic_storage_evidence_for(evidence, storage);
    plan->unproven_reason = generic_storage_reason_for(evidence, storage);
    return true;
}

static bool xaot_bundle_add_generic_storage_plans(XaotBundle *bundle,
                                                  const XgGlobalEvidence *evidence) {
    if (!bundle || !evidence)
        return false;
    for (uint32_t i = 0; i < evidence->ngeneric_storages; i++) {
        if (!xaot_bundle_add_generic_storage_plan(bundle, evidence, &evidence->generic_storages[i]))
            return false;
    }
    return true;
}

static uint8_t generic_code_size_action_for(const XgGenericCodeSizeSummary *size) {
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

static uint8_t generic_code_size_reason_for(const XgGlobalEvidence *evidence,
                                            const XgGenericCodeSizeSummary *size) {
    const XgGenericInstSummary *inst =
        size ? xg_global_evidence_find_generic_inst(evidence, size->generic_inst_id) : NULL;
    if (!size || !inst || (inst->flags & XG_GENERIC_INST_CONCRETE_TYPES) == 0)
        return XAOT_GENERIC_DEEPEN_UNPROVEN_MISSING_CONCRETE_TYPES;
    if (generic_code_size_action_for(size) == XAOT_GENERIC_CODESIZE_SHARE_CANONICAL_BODY &&
        (size->flags & XG_GENERIC_CODESIZE_SHARE_CANONICAL_BODY) == 0)
        return XAOT_GENERIC_DEEPEN_UNPROVEN_CODESIZE_THRESHOLD;
    return XAOT_GENERIC_DEEPEN_UNPROVEN_NONE;
}

static uint32_t generic_code_size_evidence_for(const XgGlobalEvidence *evidence,
                                               const XgGenericCodeSizeSummary *size) {
    uint32_t bits = XAOT_GENERIC_CODESIZE_EV_GLOBAL_ROW;
    if (!size)
        return bits;
    bits |= generic_deepen_inst_evidence(evidence, size->generic_inst_id) &
            XAOT_GENERIC_CODESIZE_EV_GENERIC_INST;
    if (size->body_use_id != XG_NO_ID)
        bits |= XAOT_GENERIC_CODESIZE_EV_BODY_USE;
    if (size->threshold != 0)
        bits |= XAOT_GENERIC_CODESIZE_EV_THRESHOLD;
    return bits;
}

static bool xaot_bundle_add_generic_code_size_plan(XaotBundle *bundle,
                                                   const XgGlobalEvidence *evidence,
                                                   const XgGenericCodeSizeSummary *size) {
    XaotGenericCodeSizePlan *plan;
    if (!bundle || !evidence || !size)
        return false;
    if (!reserve_plan_array(
            (void **) &bundle->generic_code_size_plans, &bundle->generic_code_size_plan_cap,
            bundle->ngeneric_code_size_plans + 1, sizeof(XaotGenericCodeSizePlan), 8))
        return false;
    plan = &bundle->generic_code_size_plans[bundle->ngeneric_code_size_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->code_size_id = size->code_size_id;
    plan->generic_inst_id = size->generic_inst_id;
    plan->module_id = size->module_id;
    plan->body_use_id = size->body_use_id;
    plan->origin_body_size_estimate = size->origin_body_size_estimate;
    plan->specialized_body_size_estimate = size->specialized_body_size_estimate;
    plan->instantiation_count = size->instantiation_count;
    plan->threshold = size->threshold;
    plan->action = generic_code_size_action_for(size);
    plan->evidence = generic_code_size_evidence_for(evidence, size);
    plan->unproven_reason = generic_code_size_reason_for(evidence, size);
    return true;
}

static bool xaot_bundle_add_generic_code_size_plans(XaotBundle *bundle,
                                                    const XgGlobalEvidence *evidence) {
    if (!bundle || !evidence)
        return false;
    for (uint32_t i = 0; i < evidence->ngeneric_code_sizes; i++) {
        if (!xaot_bundle_add_generic_code_size_plan(bundle, evidence,
                                                    &evidence->generic_code_sizes[i]))
            return false;
    }
    return true;
}

static uint8_t derive_action_for(const XgDeriveSummary *derive) {
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

static uint8_t derive_reason_for(const XgDeriveSummary *derive) {
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

static uint32_t derive_evidence_for(const XgDeriveSummary *derive) {
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

static XgFuncId derive_generated_body_func_id(const XgGlobalEvidence *evidence,
                                              const XgDeriveSummary *derive) {
    if (!evidence || !derive || derive->method_count == 0 || derive->method_start == 0)
        return XG_NO_ID;
    if (derive->method_start > evidence->nderived_methods)
        return XG_NO_ID;
    return evidence->derived_methods[derive->method_start - 1].generated_body_func_id;
}

static bool xaot_bundle_add_derive_plan(XaotBundle *bundle, const XgGlobalEvidence *evidence,
                                        const XgDeriveSummary *derive) {
    XaotDerivePlan *plan;
    if (!bundle || !evidence || !derive)
        return false;
    if (!reserve_plan_array((void **) &bundle->derive_plans, &bundle->derive_plan_cap,
                            bundle->nderive_plans + 1, sizeof(XaotDerivePlan), 8))
        return false;
    plan = &bundle->derive_plans[bundle->nderive_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->derive_id = derive->derive_id;
    plan->owner_decl_id = derive->owner_decl_id;
    plan->type_key = derive->type_key;
    plan->derive_kind = derive->derive_kind;
    plan->action = derive_action_for(derive);
    plan->field_start = derive->field_start;
    plan->field_count = derive->field_count;
    plan->method_start = derive->method_start;
    plan->method_count = derive->method_count;
    plan->sidecar_index =
        plan->action == XAOT_DERIVE_FIELD_TABLE_SIDECAR ? bundle->nderive_plans : 0;
    plan->generated_body_func_id = derive_generated_body_func_id(evidence, derive);
    plan->evidence = derive_evidence_for(derive);
    plan->unproven_reason = derive_reason_for(derive);
    return true;
}

static bool xaot_bundle_add_derive_plans(XaotBundle *bundle, const XgGlobalEvidence *evidence) {
    if (!bundle || !evidence)
        return false;
    for (uint32_t i = 0; i < evidence->nderives; i++) {
        if (!xaot_bundle_add_derive_plan(bundle, evidence, &evidence->derives[i]))
            return false;
    }
    return true;
}

static const XgDeriveSummary *find_derive_for_type_kind(const XgGlobalEvidence *evidence,
                                                        uint32_t type_key, XgDeclId owner_decl_id,
                                                        uint8_t derive_kind) {
    if (!evidence || type_key == 0)
        return NULL;
    for (uint32_t i = 0; i < evidence->nderives; i++) {
        const XgDeriveSummary *derive = &evidence->derives[i];
        if (derive->type_key == type_key && derive->owner_decl_id == owner_decl_id &&
            derive->derive_kind == derive_kind)
            return derive;
    }
    return NULL;
}

static bool derived_eq_hash_field_range_valid(const XgGlobalEvidence *evidence,
                                              const XgDeriveSummary *derive) {
    uint32_t end;
    if (!evidence || !derive)
        return false;
    if (derive->field_count == 0)
        return derive->field_start == 0;
    if (derive->field_start == 0)
        return false;
    end = derive->field_start + (uint32_t) derive->field_count - 1;
    return end >= derive->field_start && end <= evidence->nderived_fields;
}

static bool derived_eq_hash_pair_fields_match(const XgGlobalEvidence *evidence,
                                              const XgDeriveSummary *eq,
                                              const XgDeriveSummary *hash) {
    if (!evidence || !eq || !hash || eq->field_count != hash->field_count)
        return false;
    if (!derived_eq_hash_field_range_valid(evidence, eq) ||
        !derived_eq_hash_field_range_valid(evidence, hash))
        return false;
    for (uint32_t i = 0; i < eq->field_count; i++) {
        const XgDerivedFieldSummary *eq_field = &evidence->derived_fields[eq->field_start - 1 + i];
        const XgDerivedFieldSummary *hash_field =
            &evidence->derived_fields[hash->field_start - 1 + i];
        if (eq_field->field_ordinal != hash_field->field_ordinal ||
            eq_field->name_id != hash_field->name_id ||
            eq_field->type_key != hash_field->type_key ||
            eq_field->source_field_id != hash_field->source_field_id ||
            eq_field->flags != hash_field->flags)
            return false;
    }
    return true;
}

static uint8_t derived_eq_hash_action_for(const XgGlobalEvidence *evidence,
                                          const XgDeriveSummary *eq, const XgDeriveSummary *hash) {
    if (!eq || !hash || eq->type_key != hash->type_key ||
        !derived_eq_hash_pair_fields_match(evidence, eq, hash))
        return XAOT_DERIVED_EQ_HASH_REJECT_UNHASHABLE;
    if (eq->method_count != 0 && hash->method_count != 0)
        return XAOT_DERIVED_EQ_HASH_DIRECT_GENERATED_CALL;
    return XAOT_DERIVED_EQ_HASH_BUILTIN_FIELDS_INLINE;
}

static uint8_t derived_eq_hash_reason_for(const XgGlobalEvidence *evidence,
                                          const XgDeriveSummary *eq, const XgDeriveSummary *hash) {
    if (!eq)
        return XAOT_EQ_HASH_UNPROVEN_MISSING_EQ;
    if (!hash)
        return XAOT_EQ_HASH_UNPROVEN_MISSING_HASH;
    if (eq->type_key != hash->type_key)
        return XAOT_EQ_HASH_UNPROVEN_TYPE_MISMATCH;
    if (!derived_eq_hash_pair_fields_match(evidence, eq, hash))
        return XAOT_EQ_HASH_UNPROVEN_FIELD_MISMATCH;
    return XAOT_EQ_HASH_UNPROVEN_NONE;
}

static uint32_t derived_eq_hash_evidence_for(const XgGlobalEvidence *evidence,
                                             const XgDeriveSummary *eq,
                                             const XgDeriveSummary *hash) {
    uint32_t bits = 0;
    if (eq)
        bits |= XAOT_EQ_HASH_EV_EQ_ROW;
    if (hash)
        bits |= XAOT_EQ_HASH_EV_HASH_ROW;
    if (eq && hash && eq->type_key == hash->type_key)
        bits |= XAOT_EQ_HASH_EV_SAME_TYPE;
    if (derived_eq_hash_pair_fields_match(evidence, eq, hash))
        bits |= XAOT_EQ_HASH_EV_SAME_FIELDS;
    if (eq && eq->method_count != 0)
        bits |= XAOT_EQ_HASH_EV_EQ_BODY;
    if (hash && hash->method_count != 0)
        bits |= XAOT_EQ_HASH_EV_HASH_BODY;
    return bits;
}

static bool xaot_bundle_add_derived_eq_hash_plan(XaotBundle *bundle,
                                                 const XgGlobalEvidence *evidence,
                                                 const XgDeriveSummary *seed) {
    const XgDeriveSummary *eq;
    const XgDeriveSummary *hash;
    XaotDerivedEqHashPlan *plan;
    if (!bundle || !evidence || !seed)
        return false;
    if (xaot_bundle_find_derived_eq_hash_plan(bundle, seed->type_key))
        return true;
    eq = find_derive_for_type_kind(evidence, seed->type_key, seed->owner_decl_id, XG_DERIVE_EQ);
    hash = find_derive_for_type_kind(evidence, seed->type_key, seed->owner_decl_id, XG_DERIVE_HASH);
    if (!reserve_plan_array((void **) &bundle->derived_eq_hash_plans,
                            &bundle->derived_eq_hash_plan_cap, bundle->nderived_eq_hash_plans + 1,
                            sizeof(XaotDerivedEqHashPlan), 8))
        return false;
    plan = &bundle->derived_eq_hash_plans[bundle->nderived_eq_hash_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->owner_decl_id = seed->owner_decl_id;
    plan->type_key = seed->type_key;
    plan->eq_derive_id = eq ? eq->derive_id : XG_NO_ID;
    plan->hash_derive_id = hash ? hash->derive_id : XG_NO_ID;
    plan->field_start = eq ? eq->field_start : (hash ? hash->field_start : 0);
    plan->field_count = eq ? eq->field_count : (hash ? hash->field_count : 0);
    plan->eq_body_func_id = derive_generated_body_func_id(evidence, eq);
    plan->hash_body_func_id = derive_generated_body_func_id(evidence, hash);
    plan->action = derived_eq_hash_action_for(evidence, eq, hash);
    plan->evidence = derived_eq_hash_evidence_for(evidence, eq, hash);
    plan->unproven_reason = derived_eq_hash_reason_for(evidence, eq, hash);
    return true;
}

static bool xaot_bundle_add_derived_eq_hash_plans(XaotBundle *bundle,
                                                  const XgGlobalEvidence *evidence) {
    if (!bundle || !evidence)
        return false;
    for (uint32_t i = 0; i < evidence->nderives; i++) {
        const XgDeriveSummary *derive = &evidence->derives[i];
        if (derive->derive_kind != XG_DERIVE_EQ && derive->derive_kind != XG_DERIVE_HASH)
            continue;
        if (!xaot_bundle_add_derived_eq_hash_plan(bundle, evidence, derive))
            return false;
    }
    return true;
}

static uint8_t derived_clone_action_for(const XgDeriveSummary *clone) {
    if (!clone || clone->derive_kind != XG_DERIVE_CLONE)
        return XAOT_DERIVED_CLONE_REJECT;
    if (clone->method_count != 0)
        return XAOT_DERIVED_CLONE_DIRECT_GENERATED_CALL;
    if (clone->field_count == 0)
        return XAOT_DERIVED_CLONE_BITWISE_COPY;
    return XAOT_DERIVED_CLONE_FIELDWISE_COPY;
}

static uint8_t derived_clone_reason_for(const XgDeriveSummary *clone) {
    if (!clone || clone->derive_kind != XG_DERIVE_CLONE)
        return XAOT_CLONE_UNPROVEN_MISSING_CLONE;
    return XAOT_CLONE_UNPROVEN_NONE;
}

static uint32_t derived_clone_evidence_for(const XgDeriveSummary *clone) {
    uint32_t bits = 0;
    if (!clone || clone->derive_kind != XG_DERIVE_CLONE)
        return bits;
    bits |= XAOT_CLONE_EV_CLONE_ROW;
    if (clone->field_count != 0)
        bits |= XAOT_CLONE_EV_FIELD_TABLE;
    if (clone->method_count != 0)
        bits |= XAOT_CLONE_EV_GENERATED_BODY;
    return bits;
}

static bool xaot_bundle_add_derived_clone_plan(XaotBundle *bundle, const XgGlobalEvidence *evidence,
                                               const XgDeriveSummary *clone) {
    XaotDerivedClonePlan *plan;
    if (!bundle || !evidence || !clone)
        return false;
    if (clone->derive_kind != XG_DERIVE_CLONE)
        return true;
    if (xaot_bundle_find_derived_clone_plan(bundle, clone->type_key))
        return true;
    if (!reserve_plan_array((void **) &bundle->derived_clone_plans, &bundle->derived_clone_plan_cap,
                            bundle->nderived_clone_plans + 1, sizeof(XaotDerivedClonePlan), 8))
        return false;
    plan = &bundle->derived_clone_plans[bundle->nderived_clone_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->owner_decl_id = clone->owner_decl_id;
    plan->type_key = clone->type_key;
    plan->clone_derive_id = clone->derive_id;
    plan->field_start = clone->field_start;
    plan->field_count = clone->field_count;
    plan->clone_body_func_id = derive_generated_body_func_id(evidence, clone);
    plan->transfer_plan_id = XG_NO_ID;
    plan->action = derived_clone_action_for(clone);
    plan->evidence = derived_clone_evidence_for(clone);
    plan->unproven_reason = derived_clone_reason_for(clone);
    return true;
}

static bool xaot_bundle_add_derived_clone_plans(XaotBundle *bundle,
                                                const XgGlobalEvidence *evidence) {
    if (!bundle || !evidence)
        return false;
    for (uint32_t i = 0; i < evidence->nderives; i++) {
        const XgDeriveSummary *derive = &evidence->derives[i];
        if (derive->derive_kind != XG_DERIVE_CLONE)
            continue;
        if (!xaot_bundle_add_derived_clone_plan(bundle, evidence, derive))
            return false;
    }
    return true;
}

static uint8_t json_shape_action_for(const XgJsonShapeSummary *shape) {
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

static uint8_t json_shape_reason_for(const XgJsonShapeSummary *shape) {
    if (!shape)
        return XAOT_JSON_UNPROVEN_INVALID_KIND;
    switch ((XgJsonShapeKind) shape->shape_kind) {
        case XG_JSON_SHAPE_OPEN:
        case XG_JSON_SHAPE_SHAPED:
        case XG_JSON_SHAPE_RECORD_BRIDGE:
            return XAOT_JSON_UNPROVEN_NONE;
        default:
            return XAOT_JSON_UNPROVEN_INVALID_KIND;
    }
}

static uint32_t json_shape_evidence_for(const XgJsonShapeSummary *shape) {
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

static bool xaot_bundle_add_json_shape_plan(XaotBundle *bundle, const XgJsonShapeSummary *shape) {
    XaotJsonShapePlan *plan;
    if (!bundle || !shape)
        return false;
    if (!reserve_plan_array((void **) &bundle->json_shape_plans, &bundle->json_shape_plan_cap,
                            bundle->njson_shape_plans + 1, sizeof(XaotJsonShapePlan), 8))
        return false;
    plan = &bundle->json_shape_plans[bundle->njson_shape_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->json_shape_id = shape->json_shape_id;
    plan->module_id = shape->module_id;
    plan->owner_func_id = shape->owner_func_id;
    plan->type_key = shape->type_key;
    plan->field_name_start = shape->field_name_start;
    plan->field_count = shape->field_count;
    plan->shape_kind = shape->shape_kind;
    plan->action = json_shape_action_for(shape);
    plan->evidence = json_shape_evidence_for(shape);
    plan->unproven_reason = json_shape_reason_for(shape);
    plan->shape_hash = shape->shape_hash;
    return true;
}

static bool xaot_bundle_add_json_shape_plans(XaotBundle *bundle, const XgGlobalEvidence *evidence) {
    if (!bundle || !evidence)
        return false;
    for (uint32_t i = 0; i < evidence->njson_shapes; i++) {
        if (!xaot_bundle_add_json_shape_plan(bundle, &evidence->json_shapes[i]))
            return false;
    }
    return true;
}

static bool json_access_kind_valid(uint8_t kind) {
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

static uint8_t json_access_action_for(const XgGlobalEvidence *evidence,
                                      const XgJsonAccessSummary *access) {
    const XgJsonShapeSummary *shape;
    if (!access || !json_access_kind_valid(access->access_kind))
        return XAOT_JSON_ACCESS_REJECT;
    if ((access->flags & XG_JSON_ACCESS_COMPUTED_KEY) != 0)
        return access->receiver_shape_id != XG_NO_ID ? XAOT_JSON_ACCESS_COMPUTED_KEY_GUARD
                                                     : XAOT_JSON_ACCESS_DYNAMIC_LOOKUP;
    if (access->key_name_id == 0)
        return XAOT_JSON_ACCESS_DYNAMIC_LOOKUP;
    if (access->receiver_shape_id == XG_NO_ID)
        return XAOT_JSON_ACCESS_DYNAMIC_LOOKUP;
    shape = xg_global_evidence_find_json_shape(evidence, access->receiver_shape_id);
    if (!shape || access->field_ordinal >= shape->field_count)
        return XAOT_JSON_ACCESS_REJECT;
    if (shape->shape_kind == XG_JSON_SHAPE_OPEN)
        return XAOT_JSON_ACCESS_SHAPE_GUARD_INDEX;
    return (access->flags & XG_JSON_ACCESS_RECEIVER_SHAPE_PROVEN) != 0
               ? XAOT_JSON_ACCESS_DIRECT_INDEX
               : XAOT_JSON_ACCESS_SHAPE_GUARD_INDEX;
}

static uint8_t json_access_reason_for(const XgGlobalEvidence *evidence,
                                      const XgJsonAccessSummary *access) {
    const XgJsonShapeSummary *shape;
    if (!access || !json_access_kind_valid(access->access_kind))
        return XAOT_JSON_UNPROVEN_INVALID_KIND;
    if ((access->flags & XG_JSON_ACCESS_COMPUTED_KEY) != 0)
        return access->receiver_shape_id != XG_NO_ID ? XAOT_JSON_UNPROVEN_NONE
                                                     : XAOT_JSON_UNPROVEN_COMPUTED_KEY;
    if (access->key_name_id == 0)
        return XAOT_JSON_UNPROVEN_COMPUTED_KEY;
    if (access->receiver_shape_id == XG_NO_ID)
        return XAOT_JSON_UNPROVEN_RECEIVER_SHAPE_UNKNOWN;
    shape = xg_global_evidence_find_json_shape(evidence, access->receiver_shape_id);
    if (!shape || access->field_ordinal >= shape->field_count)
        return XAOT_JSON_UNPROVEN_STALE_SHAPE;
    return XAOT_JSON_UNPROVEN_NONE;
}

static uint32_t json_access_evidence_for(const XgGlobalEvidence *evidence,
                                         const XgJsonAccessSummary *access) {
    const XgJsonShapeSummary *shape;
    uint32_t evidence_bits = XAOT_JSON_EV_GLOBAL_ROW;
    if (!access)
        return evidence_bits;
    if ((access->flags & XG_JSON_ACCESS_STATIC_KEY) != 0 && access->key_name_id != 0)
        evidence_bits |= XAOT_JSON_EV_STATIC_KEY;
    if (access->receiver_shape_id != XG_NO_ID)
        evidence_bits |= XAOT_JSON_EV_RECEIVER_SHAPE;
    shape = xg_global_evidence_find_json_shape(evidence, access->receiver_shape_id);
    if (shape && access->field_ordinal < shape->field_count)
        evidence_bits |= XAOT_JSON_EV_FIELD_INDEX;
    return evidence_bits;
}

static bool xaot_bundle_add_json_access_plan(XaotBundle *bundle, const XgGlobalEvidence *evidence,
                                             const XgJsonAccessSummary *access) {
    XaotJsonAccessPlan *plan;
    if (!bundle || !evidence || !access)
        return false;
    if (!reserve_plan_array((void **) &bundle->json_access_plans, &bundle->json_access_plan_cap,
                            bundle->njson_access_plans + 1, sizeof(XaotJsonAccessPlan), 8))
        return false;
    plan = &bundle->json_access_plans[bundle->njson_access_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->json_access_id = access->json_access_id;
    plan->module_id = access->module_id;
    plan->owner_func_id = access->owner_func_id;
    plan->receiver_shape_id = access->receiver_shape_id;
    plan->key_name_id = access->key_name_id;
    plan->result_type_key = access->result_type_key;
    plan->field_ordinal = access->field_ordinal;
    plan->access_kind = access->access_kind;
    plan->action = json_access_action_for(evidence, access);
    plan->evidence = json_access_evidence_for(evidence, access);
    plan->unproven_reason = json_access_reason_for(evidence, access);
    return true;
}

static bool xaot_bundle_add_json_access_plans(XaotBundle *bundle,
                                              const XgGlobalEvidence *evidence) {
    if (!bundle || !evidence)
        return false;
    for (uint32_t i = 0; i < evidence->njson_accesses; i++) {
        if (!xaot_bundle_add_json_access_plan(bundle, evidence, &evidence->json_accesses[i]))
            return false;
    }
    return true;
}

static bool json_codec_kind_valid(uint8_t kind) {
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

static uint8_t json_codec_action_for(const XgJsonCodecSummary *codec) {
    if (!codec || !json_codec_kind_valid(codec->codec_kind))
        return XAOT_JSON_CODEC_REJECT;
    switch ((XgJsonCodecKind) codec->codec_kind) {
        case XG_JSON_CODEC_PARSE:
            return XAOT_JSON_CODEC_PARSE_RUNTIME_DIRECT;
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

static uint8_t json_codec_reason_for(const XgJsonCodecSummary *codec) {
    if (!codec || !json_codec_kind_valid(codec->codec_kind))
        return XAOT_JSON_UNPROVEN_INVALID_KIND;
    if (codec->codec_kind == XG_JSON_CODEC_DECODE &&
        (codec->target_type_key == 0 || (codec->flags & XG_JSON_CODEC_HAS_TARGET_TYPE) == 0))
        return XAOT_JSON_UNPROVEN_MISSING_TARGET_TYPE;
    return XAOT_JSON_UNPROVEN_NONE;
}

static uint32_t json_codec_evidence_for(const XgJsonCodecSummary *codec) {
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

static bool xaot_bundle_add_json_codec_plan(XaotBundle *bundle, const XgJsonCodecSummary *codec) {
    XaotJsonCodecPlan *plan;
    if (!bundle || !codec)
        return false;
    if (!reserve_plan_array((void **) &bundle->json_codec_plans, &bundle->json_codec_plan_cap,
                            bundle->njson_codec_plans + 1, sizeof(XaotJsonCodecPlan), 8))
        return false;
    plan = &bundle->json_codec_plans[bundle->njson_codec_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->codec_id = codec->codec_id;
    plan->module_id = codec->module_id;
    plan->owner_func_id = codec->owner_func_id;
    plan->source_span_id = codec->source_span_id;
    plan->codec_kind = codec->codec_kind;
    plan->action = json_codec_action_for(codec);
    plan->input_type_key = codec->input_type_key;
    plan->target_type_key = codec->target_type_key;
    plan->input_shape_id = codec->input_shape_id;
    plan->output_shape_id = codec->output_shape_id;
    plan->field_count = codec->field_count;
    plan->evidence = json_codec_evidence_for(codec);
    plan->unproven_reason = json_codec_reason_for(codec);
    return true;
}

static bool xaot_bundle_add_json_codec_plans(XaotBundle *bundle, const XgGlobalEvidence *evidence) {
    if (!bundle || !evidence)
        return false;
    for (uint32_t i = 0; i < evidence->njson_codecs; i++) {
        if (!xaot_bundle_add_json_codec_plan(bundle, &evidence->json_codecs[i]))
            return false;
    }
    return true;
}

static bool record_shape_kind_valid(uint8_t kind) {
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

static bool record_access_kind_valid(uint8_t kind) {
    switch ((XgRecordAccessKind) kind) {
        case XG_RECORD_ACCESS_FIELD_GET:
        case XG_RECORD_ACCESS_FIELD_SET:
        case XG_RECORD_ACCESS_DESTRUCTURE:
            return true;
        default:
            return false;
    }
}

static uint8_t record_shape_action_for(const XgRecordShapeSummary *shape) {
    if (!shape || !record_shape_kind_valid(shape->shape_kind))
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

static uint8_t record_shape_reason_for(const XgRecordShapeSummary *shape) {
    return shape && record_shape_kind_valid(shape->shape_kind) ? XAOT_RECORD_UNPROVEN_NONE
                                                               : XAOT_RECORD_UNPROVEN_INVALID_KIND;
}

static uint32_t record_shape_evidence_for(const XgRecordShapeSummary *shape) {
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

static bool xaot_bundle_add_record_shape_plan(XaotBundle *bundle,
                                              const XgRecordShapeSummary *shape) {
    XaotRecordShapePlan *plan;
    if (!bundle || !shape)
        return false;
    if (!reserve_plan_array((void **) &bundle->record_shape_plans, &bundle->record_shape_plan_cap,
                            bundle->nrecord_shape_plans + 1, sizeof(XaotRecordShapePlan), 8))
        return false;
    plan = &bundle->record_shape_plans[bundle->nrecord_shape_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->record_shape_id = shape->record_shape_id;
    plan->module_id = shape->module_id;
    plan->owner_func_id = shape->owner_func_id;
    plan->type_key = shape->type_key;
    plan->field_name_start = shape->field_name_start;
    plan->field_count = shape->field_count;
    plan->shape_kind = shape->shape_kind;
    plan->action = record_shape_action_for(shape);
    plan->evidence = record_shape_evidence_for(shape);
    plan->unproven_reason = record_shape_reason_for(shape);
    plan->shape_hash = shape->shape_hash;
    return true;
}

static bool xaot_bundle_add_record_shape_plans(XaotBundle *bundle,
                                               const XgGlobalEvidence *evidence) {
    if (!bundle || !evidence)
        return false;
    for (uint32_t i = 0; i < evidence->nrecord_shapes; i++) {
        if (!xaot_bundle_add_record_shape_plan(bundle, &evidence->record_shapes[i]))
            return false;
    }
    return true;
}

static uint8_t record_access_action_for(const XgGlobalEvidence *evidence,
                                        const XgRecordAccessSummary *access) {
    const XgRecordShapeSummary *shape;
    if (!access || !record_access_kind_valid(access->access_kind))
        return XAOT_RECORD_ACCESS_REJECT;
    if ((access->flags & XG_RECORD_ACCESS_STATIC_FIELD) == 0 || access->field_name_id == 0)
        return XAOT_RECORD_ACCESS_REJECT;
    if (access->receiver_shape_id == XG_NO_ID)
        return XAOT_RECORD_ACCESS_CHECKED_FIELD;
    shape = xg_global_evidence_find_record_shape(evidence, access->receiver_shape_id);
    if (!shape || access->field_ordinal >= shape->field_count)
        return XAOT_RECORD_ACCESS_REJECT;
    if (access->access_kind == XG_RECORD_ACCESS_DESTRUCTURE)
        return XAOT_RECORD_ACCESS_COPY_DESTRUCTURE;
    return (access->flags & XG_RECORD_ACCESS_RECEIVER_SHAPE_PROVEN) != 0
               ? XAOT_RECORD_ACCESS_DIRECT_FIELD
               : XAOT_RECORD_ACCESS_CHECKED_FIELD;
}

static uint8_t record_access_reason_for(const XgGlobalEvidence *evidence,
                                        const XgRecordAccessSummary *access) {
    const XgRecordShapeSummary *shape;
    if (!access || !record_access_kind_valid(access->access_kind))
        return XAOT_RECORD_UNPROVEN_INVALID_KIND;
    if ((access->flags & XG_RECORD_ACCESS_STATIC_FIELD) == 0 || access->field_name_id == 0)
        return XAOT_RECORD_UNPROVEN_DYNAMIC_FIELD;
    if (access->receiver_shape_id == XG_NO_ID)
        return XAOT_RECORD_UNPROVEN_RECEIVER_SHAPE_UNKNOWN;
    shape = xg_global_evidence_find_record_shape(evidence, access->receiver_shape_id);
    if (!shape || access->field_ordinal >= shape->field_count)
        return XAOT_RECORD_UNPROVEN_STALE_SHAPE;
    return XAOT_RECORD_UNPROVEN_NONE;
}

static uint32_t record_access_evidence_for(const XgGlobalEvidence *evidence,
                                           const XgRecordAccessSummary *access) {
    const XgRecordShapeSummary *shape;
    uint32_t evidence_bits = XAOT_RECORD_EV_GLOBAL_ROW;
    if (!access)
        return evidence_bits;
    if ((access->flags & XG_RECORD_ACCESS_STATIC_FIELD) != 0 && access->field_name_id != 0)
        evidence_bits |= XAOT_RECORD_EV_STATIC_FIELD;
    if (access->receiver_shape_id != XG_NO_ID)
        evidence_bits |= XAOT_RECORD_EV_RECEIVER_SHAPE;
    shape = xg_global_evidence_find_record_shape(evidence, access->receiver_shape_id);
    if (shape && access->field_ordinal < shape->field_count)
        evidence_bits |= XAOT_RECORD_EV_FIELD_INDEX;
    return evidence_bits;
}

static bool xaot_bundle_add_record_access_plan(XaotBundle *bundle, const XgGlobalEvidence *evidence,
                                               const XgRecordAccessSummary *access) {
    XaotRecordAccessPlan *plan;
    if (!bundle || !evidence || !access)
        return false;
    if (!reserve_plan_array((void **) &bundle->record_access_plans, &bundle->record_access_plan_cap,
                            bundle->nrecord_access_plans + 1, sizeof(XaotRecordAccessPlan), 8))
        return false;
    plan = &bundle->record_access_plans[bundle->nrecord_access_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->record_access_id = access->record_access_id;
    plan->module_id = access->module_id;
    plan->owner_func_id = access->owner_func_id;
    plan->receiver_shape_id = access->receiver_shape_id;
    plan->field_name_id = access->field_name_id;
    plan->result_type_key = access->result_type_key;
    plan->field_ordinal = access->field_ordinal;
    plan->access_kind = access->access_kind;
    plan->action = record_access_action_for(evidence, access);
    plan->evidence = record_access_evidence_for(evidence, access);
    plan->unproven_reason = record_access_reason_for(evidence, access);
    return true;
}

static bool xaot_bundle_add_record_access_plans(XaotBundle *bundle,
                                                const XgGlobalEvidence *evidence) {
    if (!bundle || !evidence)
        return false;
    for (uint32_t i = 0; i < evidence->nrecord_accesses; i++) {
        if (!xaot_bundle_add_record_access_plan(bundle, evidence, &evidence->record_accesses[i]))
            return false;
    }
    return true;
}

static bool options_action_valid(uint8_t action) {
    switch ((XgOptionsAction) action) {
        case XG_OPTIONS_DEFAULT_ELIDED:
        case XG_OPTIONS_DEFAULT_FILL_TABLE:
        case XG_OPTIONS_REQUIRED_CHECK:
        case XG_OPTIONS_CALLSITE_SPECIALIZED:
        case XG_OPTIONS_REJECT:
            return true;
        default:
            return false;
    }
}

static uint8_t options_action_for(const XgGlobalEvidence *evidence,
                                  const XgOptionsBagSummary *options) {
    if (!evidence || !options || !options_action_valid(options->action))
        return XAOT_OPTIONS_REJECT;
    if (!xg_global_evidence_find_callsite(evidence, options->callsite_id))
        return XAOT_OPTIONS_REJECT;
    if (!xg_global_evidence_find_record_shape(evidence, options->param_shape_id))
        return XAOT_OPTIONS_REJECT;
    if (options->supplied_shape_id != XG_NO_ID &&
        !xg_global_evidence_find_record_shape(evidence, options->supplied_shape_id))
        return XAOT_OPTIONS_REJECT;
    if ((options->flags & XG_OPTIONS_MISSING_REQUIRED) != 0)
        return XAOT_OPTIONS_REQUIRED_CHECK;
    if ((options->flags & XG_OPTIONS_ALL_SUPPLIED) != 0 &&
        (options->flags & XG_OPTIONS_NEEDS_DEFAULTS) == 0)
        return XAOT_OPTIONS_DEFAULT_ELIDED;
    if ((options->flags & XG_OPTIONS_NEEDS_DEFAULTS) != 0)
        return XAOT_OPTIONS_DEFAULT_FILL_TABLE;
    if ((options->flags & XG_OPTIONS_CALLSITE_PROVEN) != 0)
        return XAOT_OPTIONS_CALLSITE_SPECIALIZED;
    return XAOT_OPTIONS_REJECT;
}

static uint8_t options_reason_for(const XgGlobalEvidence *evidence,
                                  const XgOptionsBagSummary *options) {
    if (!evidence || !options || !options_action_valid(options->action))
        return XAOT_OPTIONS_UNPROVEN_INVALID_ACTION;
    if (!xg_global_evidence_find_callsite(evidence, options->callsite_id))
        return XAOT_OPTIONS_UNPROVEN_MISSING_CALLSITE;
    if (!xg_global_evidence_find_record_shape(evidence, options->param_shape_id))
        return XAOT_OPTIONS_UNPROVEN_STALE_SHAPE;
    if (options->supplied_shape_id != XG_NO_ID &&
        !xg_global_evidence_find_record_shape(evidence, options->supplied_shape_id))
        return XAOT_OPTIONS_UNPROVEN_STALE_SHAPE;
    return XAOT_OPTIONS_UNPROVEN_NONE;
}

static uint32_t options_evidence_for(const XgGlobalEvidence *evidence,
                                     const XgOptionsBagSummary *options) {
    uint32_t evidence_bits = XAOT_OPTIONS_EV_GLOBAL_ROW;
    if (!evidence || !options)
        return evidence_bits;
    if (xg_global_evidence_find_callsite(evidence, options->callsite_id))
        evidence_bits |= XAOT_OPTIONS_EV_CALLSITE;
    if (xg_global_evidence_find_record_shape(evidence, options->param_shape_id))
        evidence_bits |= XAOT_OPTIONS_EV_PARAM_SHAPE;
    if (options->supplied_shape_id != XG_NO_ID &&
        xg_global_evidence_find_record_shape(evidence, options->supplied_shape_id))
        evidence_bits |= XAOT_OPTIONS_EV_SUPPLIED_SHAPE;
    if (options->default_field_mask_id != 0)
        evidence_bits |= XAOT_OPTIONS_EV_DEFAULT_MASK;
    if (options->required_field_mask_id != 0)
        evidence_bits |= XAOT_OPTIONS_EV_REQUIRED_MASK;
    return evidence_bits;
}

static bool xaot_bundle_add_options_plan(XaotBundle *bundle, const XgGlobalEvidence *evidence,
                                         const XgOptionsBagSummary *options) {
    XaotOptionsPlan *plan;
    if (!bundle || !evidence || !options)
        return false;
    if (!reserve_plan_array((void **) &bundle->options_plans, &bundle->options_plan_cap,
                            bundle->noptions_plans + 1, sizeof(XaotOptionsPlan), 8))
        return false;
    plan = &bundle->options_plans[bundle->noptions_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->options_id = options->options_id;
    plan->module_id = options->module_id;
    plan->owner_func_id = options->owner_func_id;
    plan->callsite_id = options->callsite_id;
    plan->param_shape_id = options->param_shape_id;
    plan->supplied_shape_id = options->supplied_shape_id;
    plan->supplied_field_mask_id = options->supplied_field_mask_id;
    plan->default_field_mask_id = options->default_field_mask_id;
    plan->required_field_mask_id = options->required_field_mask_id;
    plan->supplied_count = options->supplied_count;
    plan->default_count = options->default_count;
    plan->required_count = options->required_count;
    plan->action = options_action_for(evidence, options);
    plan->evidence = options_evidence_for(evidence, options);
    plan->unproven_reason = options_reason_for(evidence, options);
    return true;
}

static bool xaot_bundle_add_options_plans(XaotBundle *bundle, const XgGlobalEvidence *evidence) {
    if (!bundle || !evidence)
        return false;
    for (uint32_t i = 0; i < evidence->noptions_bags; i++) {
        if (!xaot_bundle_add_options_plan(bundle, evidence, &evidence->options_bags[i]))
            return false;
    }
    return true;
}

static bool map_container_kind_valid(uint8_t kind) {
    return kind == XG_MAP_CONTAINER_MAP || kind == XG_MAP_CONTAINER_SET;
}

static bool map_shape_source_valid(uint8_t source) {
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

static bool key_access_op_valid(uint8_t op) {
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

static bool hash_eq_kind_valid(uint8_t kind) {
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

static bool map_shape_value_type_supports_bool_direct(uint32_t value_type_key) {
    return value_type_key == xg_synthetic_type_key(XR_TREF_INT) ||
           value_type_key == xg_synthetic_width_type_key(XR_TREF_INT_WIDTH, XR_TREF_NW_I64) ||
           value_type_key == xg_synthetic_width_type_key(XR_TREF_FLOAT_WIDTH, XR_TREF_NW_F32);
}

static bool map_shape_supports_bool_direct(const XgMapShapeSummary *shape) {
    return shape && shape->container_kind == XG_MAP_CONTAINER_MAP &&
           shape->key_type_key == xg_synthetic_type_key(XR_TREF_BOOL) &&
           map_shape_value_type_supports_bool_direct(shape->value_type_key) &&
           shape->entry_count > 0 && shape->entry_count <= 2;
}

static uint8_t map_shape_action_for(const XgMapShapeSummary *shape) {
    if (!shape || !map_container_kind_valid(shape->container_kind) ||
        !map_shape_source_valid(shape->source))
        return XAOT_MAP_SHAPE_REJECT;
    if ((shape->flags & XG_MAP_SHAPE_BOOL_DIRECT) != 0 && !map_shape_supports_bool_direct(shape))
        return XAOT_MAP_SHAPE_REJECT;
    if ((shape->flags & (XG_MAP_SHAPE_STATIC | XG_MAP_SHAPE_READONLY)) ==
        (XG_MAP_SHAPE_STATIC | XG_MAP_SHAPE_READONLY))
        return XAOT_MAP_SHAPE_READONLY_STATIC_TABLE;
    if ((shape->flags & XG_MAP_SHAPE_DENSE_ENUM) != 0)
        return XAOT_MAP_SHAPE_DENSE_ENUM_TABLE;
    if ((shape->flags & XG_MAP_SHAPE_DENSE_INT) != 0)
        return XAOT_MAP_SHAPE_DENSE_INT_TABLE;
    if ((shape->flags & XG_MAP_SHAPE_BOOL_DIRECT) != 0 && map_shape_supports_bool_direct(shape))
        return XAOT_MAP_SHAPE_BOOL_DIRECT;
    if ((shape->flags & XG_MAP_SHAPE_SMALL) != 0)
        return XAOT_MAP_SHAPE_SMALL_INLINE;
    if ((shape->flags & XG_MAP_SHAPE_LITERAL) != 0 || shape->source == XG_MAP_SHAPE_SRC_LITERAL)
        return XAOT_MAP_SHAPE_PREALLOC_HASH;
    return XAOT_MAP_SHAPE_RUNTIME_HASH;
}

static uint8_t map_shape_reason_for(const XgMapShapeSummary *shape) {
    return shape && map_container_kind_valid(shape->container_kind) &&
                   map_shape_source_valid(shape->source)
               ? XAOT_MAP_UNPROVEN_NONE
               : XAOT_MAP_UNPROVEN_INVALID_KIND;
}

static uint32_t map_shape_evidence_for(const XgMapShapeSummary *shape) {
    uint32_t evidence = XAOT_MAP_EV_GLOBAL_ROW;
    if (!shape)
        return evidence;
    if ((shape->flags & XG_MAP_SHAPE_LITERAL) != 0 || shape->source == XG_MAP_SHAPE_SRC_LITERAL)
        evidence |= XAOT_MAP_EV_LITERAL;
    if ((shape->flags & (XG_MAP_SHAPE_DENSE_ENUM | XG_MAP_SHAPE_DENSE_INT)) != 0)
        evidence |= XAOT_MAP_EV_DENSE_DOMAIN;
    if ((shape->flags & XG_MAP_SHAPE_SMALL) != 0)
        evidence |= XAOT_MAP_EV_SMALL;
    if ((shape->flags & XG_MAP_SHAPE_BOOL_DIRECT) != 0)
        evidence |= XAOT_MAP_EV_BOOL_DOMAIN;
    return evidence;
}

static bool xaot_bundle_add_map_shape_plan(XaotBundle *bundle, const XgMapShapeSummary *shape) {
    XaotMapShapePlan *plan;
    if (!bundle || !shape)
        return false;
    if (!reserve_plan_array((void **) &bundle->map_shape_plans, &bundle->map_shape_plan_cap,
                            bundle->nmap_shape_plans + 1, sizeof(XaotMapShapePlan), 8))
        return false;
    plan = &bundle->map_shape_plans[bundle->nmap_shape_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->shape_id = shape->shape_id;
    plan->module_id = shape->module_id;
    plan->owner_func_id = shape->owner_func_id;
    plan->container_kind = shape->container_kind;
    plan->source = shape->source;
    plan->key_type_key = shape->key_type_key;
    plan->value_type_key = shape->value_type_key;
    plan->entry_start = shape->entry_start;
    plan->entry_count = shape->entry_count;
    plan->literal_count = shape->literal_count;
    plan->action = map_shape_action_for(shape);
    plan->evidence = map_shape_evidence_for(shape);
    plan->unproven_reason = map_shape_reason_for(shape);
    plan->shape_hash = shape->shape_hash;
    return true;
}

static bool xaot_bundle_add_map_shape_plans(XaotBundle *bundle, const XgGlobalEvidence *evidence) {
    if (!bundle || !evidence)
        return false;
    for (uint32_t i = 0; i < evidence->nmap_shapes; i++) {
        if (!xaot_bundle_add_map_shape_plan(bundle, &evidence->map_shapes[i]))
            return false;
    }
    return true;
}

static uint8_t hash_eq_action_for(const XgHashEqSummary *hash_eq) {
    if (!hash_eq || !hash_eq_kind_valid(hash_eq->kind) || hash_eq->type_key == 0)
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

static uint8_t hash_eq_reason_for(const XgHashEqSummary *hash_eq) {
    if (!hash_eq || !hash_eq_kind_valid(hash_eq->kind) || hash_eq->type_key == 0)
        return XAOT_MAP_UNPROVEN_INVALID_KIND;
    return hash_eq_action_for(hash_eq) == XAOT_HASH_EQ_DYNAMIC_REJECT ? XAOT_MAP_UNPROVEN_UNHASHABLE
                                                                      : XAOT_MAP_UNPROVEN_NONE;
}

static uint32_t hash_eq_evidence_for(const XgHashEqSummary *hash_eq) {
    uint32_t evidence = XAOT_MAP_EV_GLOBAL_ROW;
    if (!hash_eq)
        return evidence;
    if (hash_eq_action_for(hash_eq) != XAOT_HASH_EQ_DYNAMIC_REJECT)
        evidence |= XAOT_MAP_EV_HASH_EQ;
    return evidence;
}

static bool xaot_bundle_add_hash_eq_plan(XaotBundle *bundle, const XgHashEqSummary *hash_eq) {
    XaotHashEqPlan *plan;
    if (!bundle || !hash_eq)
        return false;
    if (!reserve_plan_array((void **) &bundle->hash_eq_plans, &bundle->hash_eq_plan_cap,
                            bundle->nhash_eq_plans + 1, sizeof(XaotHashEqPlan), 8))
        return false;
    plan = &bundle->hash_eq_plans[bundle->nhash_eq_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->hash_eq_id = hash_eq->hash_eq_id;
    plan->type_key = hash_eq->type_key;
    plan->kind = hash_eq->kind;
    plan->eq_derive_id = hash_eq->eq_derive_id;
    plan->hash_derive_id = hash_eq->hash_derive_id;
    plan->eq_func_id = hash_eq->eq_func_id;
    plan->hash_func_id = hash_eq->hash_func_id;
    plan->action = hash_eq_action_for(hash_eq);
    plan->evidence = hash_eq_evidence_for(hash_eq);
    plan->unproven_reason = hash_eq_reason_for(hash_eq);
    return true;
}

static bool xaot_bundle_add_hash_eq_plans(XaotBundle *bundle, const XgGlobalEvidence *evidence) {
    if (!bundle || !evidence)
        return false;
    for (uint32_t i = 0; i < evidence->nhash_eqs; i++) {
        if (!xaot_bundle_add_hash_eq_plan(bundle, &evidence->hash_eqs[i]))
            return false;
    }
    return true;
}

static uint8_t key_access_action_for(const XgGlobalEvidence *evidence,
                                     const XgKeyAccessSummary *access) {
    const XgMapShapeSummary *shape = NULL;
    const XgHashEqSummary *hash_eq = NULL;
    if (!access || !map_container_kind_valid(access->container_kind) ||
        !key_access_op_valid(access->op))
        return XAOT_KEY_ACCESS_REJECT;
    if (access->receiver_shape_id != XG_NO_ID) {
        shape = xg_global_evidence_find_map_shape(evidence, access->receiver_shape_id);
        if (!shape)
            return XAOT_KEY_ACCESS_REJECT;
        bool lookup_op = access->op == XG_KEY_ACCESS_GET || access->op == XG_KEY_ACCESS_INDEX_GET ||
                         access->op == XG_KEY_ACCESS_HAS;
        if (lookup_op && access->container_kind == XG_MAP_CONTAINER_MAP &&
            (shape->flags & XG_MAP_SHAPE_BOOL_DIRECT) != 0 && map_shape_supports_bool_direct(shape))
            return XAOT_KEY_ACCESS_BOOL_DIRECT_LOOKUP;
        if (lookup_op && (shape->flags & (XG_MAP_SHAPE_DENSE_ENUM | XG_MAP_SHAPE_DENSE_INT)) != 0)
            return XAOT_KEY_ACCESS_DIRECT_DENSE_INDEX;
        if (lookup_op && (shape->flags & XG_MAP_SHAPE_SMALL) != 0)
            return XAOT_KEY_ACCESS_INLINE_SMALL_SCAN;
    }
    hash_eq = xg_global_evidence_find_hash_eq(evidence, access->key_type_key);
    if (!hash_eq || hash_eq_action_for(hash_eq) == XAOT_HASH_EQ_DYNAMIC_REJECT)
        return XAOT_KEY_ACCESS_GENERIC_HASH_LOOKUP;
    return (access->flags & XG_KEY_ACCESS_CONST_KEY) != 0 && access->key_const_id != 0
               ? XAOT_KEY_ACCESS_PREHASHED_LOOKUP
               : XAOT_KEY_ACCESS_SPECIALIZED_HASH_LOOKUP;
}

static uint8_t key_access_reason_for(const XgGlobalEvidence *evidence,
                                     const XgKeyAccessSummary *access) {
    const XgHashEqSummary *hash_eq;
    if (!access || !map_container_kind_valid(access->container_kind) ||
        !key_access_op_valid(access->op))
        return XAOT_MAP_UNPROVEN_INVALID_KIND;
    if (access->receiver_shape_id != XG_NO_ID &&
        !xg_global_evidence_find_map_shape(evidence, access->receiver_shape_id))
        return XAOT_MAP_UNPROVEN_MISSING_SHAPE;
    hash_eq = xg_global_evidence_find_hash_eq(evidence, access->key_type_key);
    if (!hash_eq)
        return XAOT_MAP_UNPROVEN_MISSING_HASH_EQ;
    if (hash_eq_action_for(hash_eq) == XAOT_HASH_EQ_DYNAMIC_REJECT)
        return XAOT_MAP_UNPROVEN_UNHASHABLE;
    return XAOT_MAP_UNPROVEN_NONE;
}

static uint32_t key_access_evidence_for(const XgGlobalEvidence *evidence,
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
    shape = xg_global_evidence_find_map_shape(evidence, access->receiver_shape_id);
    bool lookup_op = access->op == XG_KEY_ACCESS_GET || access->op == XG_KEY_ACCESS_INDEX_GET ||
                     access->op == XG_KEY_ACCESS_HAS;
    if (lookup_op && shape) {
        if ((shape->flags & (XG_MAP_SHAPE_DENSE_ENUM | XG_MAP_SHAPE_DENSE_INT)) != 0)
            bits |= XAOT_MAP_EV_DENSE_DOMAIN;
        if ((shape->flags & XG_MAP_SHAPE_SMALL) != 0)
            bits |= XAOT_MAP_EV_SMALL;
        if ((shape->flags & XG_MAP_SHAPE_BOOL_DIRECT) != 0 && map_shape_supports_bool_direct(shape))
            bits |= XAOT_MAP_EV_BOOL_DOMAIN;
    }
    hash_eq = xg_global_evidence_find_hash_eq(evidence, access->key_type_key);
    if (hash_eq && hash_eq_action_for(hash_eq) != XAOT_HASH_EQ_DYNAMIC_REJECT)
        bits |= XAOT_MAP_EV_HASH_EQ;
    return bits;
}

static bool xaot_bundle_add_key_access_plan(XaotBundle *bundle, const XgGlobalEvidence *evidence,
                                            const XgKeyAccessSummary *access) {
    XaotKeyAccessPlan *plan;
    if (!bundle || !evidence || !access)
        return false;
    if (!reserve_plan_array((void **) &bundle->key_access_plans, &bundle->key_access_plan_cap,
                            bundle->nkey_access_plans + 1, sizeof(XaotKeyAccessPlan), 8))
        return false;
    plan = &bundle->key_access_plans[bundle->nkey_access_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->access_id = access->access_id;
    plan->owner_func_id = access->owner_func_id;
    plan->source_span_id = access->source_span_id;
    plan->body_ordinal = access->body_ordinal;
    plan->container_kind = access->container_kind;
    plan->op = access->op;
    plan->receiver_shape_id = access->receiver_shape_id;
    plan->receiver_type_key = access->receiver_type_key;
    plan->key_type_key = access->key_type_key;
    plan->value_type_key = access->value_type_key;
    plan->key_const_id = access->key_const_id;
    plan->key_prehash = access->key_prehash;
    plan->action = key_access_action_for(evidence, access);
    plan->evidence = key_access_evidence_for(evidence, access);
    plan->unproven_reason = key_access_reason_for(evidence, access);
    return true;
}

static bool xaot_bundle_add_key_access_plans(XaotBundle *bundle, const XgGlobalEvidence *evidence) {
    if (!bundle || !evidence)
        return false;
    for (uint32_t i = 0; i < evidence->nkey_accesses; i++) {
        if (!xaot_bundle_add_key_access_plan(bundle, evidence, &evidence->key_accesses[i]))
            return false;
    }
    return true;
}

static bool sequence_kind_valid(uint8_t kind) {
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

static bool sequence_access_kind_valid(uint8_t kind) {
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

static uint8_t sequence_access_action_for(const XgSequenceAccessSummary *seq) {
    if (!seq || !sequence_kind_valid(seq->sequence_kind) ||
        !sequence_access_kind_valid(seq->access_kind) || seq->receiver_type_key == 0)
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

static uint8_t sequence_access_reason_for(const XgSequenceAccessSummary *seq) {
    if (!seq || !sequence_kind_valid(seq->sequence_kind) ||
        !sequence_access_kind_valid(seq->access_kind))
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

static uint32_t sequence_access_evidence_for(const XgSequenceAccessSummary *seq) {
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

static bool xaot_bundle_add_sequence_access_plan(XaotBundle *bundle,
                                                 const XgSequenceAccessSummary *seq) {
    XaotSequenceAccessPlan *plan;
    if (!bundle || !seq)
        return false;
    if (!reserve_plan_array((void **) &bundle->sequence_access_plans,
                            &bundle->sequence_access_plan_cap, bundle->nsequence_access_plans + 1,
                            sizeof(XaotSequenceAccessPlan), 8))
        return false;
    plan = &bundle->sequence_access_plans[bundle->nsequence_access_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->access_id = seq->access_id;
    plan->owner_func_id = seq->owner_func_id;
    plan->source_span_id = seq->source_span_id;
    plan->body_ordinal = seq->body_ordinal;
    plan->sequence_kind = seq->sequence_kind;
    plan->access_kind = seq->access_kind;
    plan->receiver_type_key = seq->receiver_type_key;
    plan->elem_type_key = seq->elem_type_key;
    plan->index_expr_id = seq->index_expr_id;
    plan->length_expr_id = seq->length_expr_id;
    plan->action = sequence_access_action_for(seq);
    plan->evidence = sequence_access_evidence_for(seq);
    plan->unproven_reason = sequence_access_reason_for(seq);
    return true;
}

static bool xaot_bundle_add_sequence_access_plans(XaotBundle *bundle,
                                                  const XgGlobalEvidence *evidence) {
    if (!bundle || !evidence)
        return false;
    for (uint32_t i = 0; i < evidence->nsequence_accesses; i++) {
        if (!xaot_bundle_add_sequence_access_plan(bundle, &evidence->sequence_accesses[i]))
            return false;
    }
    return true;
}

static bool capacity_op_kind_valid(uint8_t kind) {
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

static uint8_t capacity_action_for(const XgCapacityOpSummary *cap) {
    if (!cap || !sequence_kind_valid(cap->sequence_kind) || !capacity_op_kind_valid(cap->op_kind) ||
        cap->receiver_type_key == 0)
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

static uint8_t capacity_reason_for(const XgCapacityOpSummary *cap) {
    if (!cap || !sequence_kind_valid(cap->sequence_kind) || !capacity_op_kind_valid(cap->op_kind))
        return XAOT_CAPACITY_UNPROVEN_INVALID_KIND;
    if (cap->receiver_type_key == 0)
        return XAOT_CAPACITY_UNPROVEN_MISSING_RECEIVER_TYPE;
    if (capacity_action_for(cap) == XAOT_CAPACITY_CHECKED_GROW)
        return XAOT_CAPACITY_UNPROVEN_COUNT_UNKNOWN;
    return XAOT_CAPACITY_UNPROVEN_NONE;
}

static uint32_t capacity_evidence_for(const XgCapacityOpSummary *cap) {
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

static bool xaot_bundle_add_capacity_plan(XaotBundle *bundle, const XgCapacityOpSummary *cap) {
    XaotCapacityPlan *plan;
    if (!bundle || !cap)
        return false;
    if (!reserve_plan_array((void **) &bundle->capacity_plans, &bundle->capacity_plan_cap,
                            bundle->ncapacity_plans + 1, sizeof(XaotCapacityPlan), 8))
        return false;
    plan = &bundle->capacity_plans[bundle->ncapacity_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->op_id = cap->op_id;
    plan->owner_func_id = cap->owner_func_id;
    plan->source_span_id = cap->source_span_id;
    plan->body_ordinal = cap->body_ordinal;
    plan->sequence_kind = cap->sequence_kind;
    plan->op_kind = cap->op_kind;
    plan->receiver_type_key = cap->receiver_type_key;
    plan->elem_type_key = cap->elem_type_key;
    plan->count_expr_id = cap->count_expr_id;
    plan->loop_id = cap->loop_id;
    plan->action = capacity_action_for(cap);
    plan->evidence = capacity_evidence_for(cap);
    plan->unproven_reason = capacity_reason_for(cap);
    return true;
}

static bool xaot_bundle_add_capacity_plans(XaotBundle *bundle, const XgGlobalEvidence *evidence) {
    if (!bundle || !evidence)
        return false;
    for (uint32_t i = 0; i < evidence->ncapacity_ops; i++) {
        if (!xaot_bundle_add_capacity_plan(bundle, &evidence->capacity_ops[i]))
            return false;
    }
    return true;
}

static bool bulk_op_kind_valid(uint8_t kind) {
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

static uint8_t bulk_action_for(const XgBulkOpSummary *bulk) {
    if (!bulk || !bulk_op_kind_valid(bulk->op_kind))
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

static uint8_t bulk_reason_for(const XgBulkOpSummary *bulk) {
    if (!bulk || !bulk_op_kind_valid(bulk->op_kind))
        return XAOT_BULK_UNPROVEN_INVALID_KIND;
    if (bulk->length_expr_id == 0)
        return XAOT_BULK_UNPROVEN_LENGTH_UNKNOWN;
    if ((bulk->flags & XG_BULK_WRITE_BARRIER) != 0)
        return XAOT_BULK_UNPROVEN_WRITE_BARRIER;
    if ((bulk->flags & XG_BULK_POD) == 0)
        return XAOT_BULK_UNPROVEN_NON_POD;
    return XAOT_BULK_UNPROVEN_NONE;
}

static uint32_t bulk_evidence_for(const XgBulkOpSummary *bulk) {
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

static bool xaot_bundle_add_bulk_plan(XaotBundle *bundle, const XgBulkOpSummary *bulk) {
    XaotBulkPlan *plan;
    if (!bundle || !bulk)
        return false;
    if (!reserve_plan_array((void **) &bundle->bulk_plans, &bundle->bulk_plan_cap,
                            bundle->nbulk_plans + 1, sizeof(XaotBulkPlan), 8))
        return false;
    plan = &bundle->bulk_plans[bundle->nbulk_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->op_id = bulk->op_id;
    plan->owner_func_id = bulk->owner_func_id;
    plan->source_span_id = bulk->source_span_id;
    plan->body_ordinal = bulk->body_ordinal;
    plan->op_kind = bulk->op_kind;
    plan->elem_type_key = bulk->elem_type_key;
    plan->src_type_key = bulk->src_type_key;
    plan->dst_type_key = bulk->dst_type_key;
    plan->length_expr_id = bulk->length_expr_id;
    plan->action = bulk_action_for(bulk);
    plan->evidence = bulk_evidence_for(bulk);
    plan->unproven_reason = bulk_reason_for(bulk);
    return true;
}

static bool xaot_bundle_add_bulk_plans(XaotBundle *bundle, const XgGlobalEvidence *evidence) {
    if (!bundle || !evidence)
        return false;
    for (uint32_t i = 0; i < evidence->nbulk_ops; i++) {
        if (!xaot_bundle_add_bulk_plan(bundle, &evidence->bulk_ops[i]))
            return false;
    }
    return true;
}

static bool encoding_op_kind_valid(uint8_t kind) {
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

static uint8_t encoding_action_for(const XgEncodingOpSummary *enc) {
    if (!enc || !encoding_op_kind_valid(enc->op_kind))
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

static uint8_t encoding_reason_for(const XgEncodingOpSummary *enc) {
    if (!enc || !encoding_op_kind_valid(enc->op_kind))
        return XAOT_ENCODING_UNPROVEN_INVALID_KIND;
    if (encoding_action_for(enc) == XAOT_ENCODING_RUNTIME_VALIDATE &&
        (enc->op_kind == XG_ENCODING_BYTES_TO_STRING || enc->op_kind == XG_ENCODING_UTF8_COUNT))
        return XAOT_ENCODING_UNPROVEN_RAW_BYTES_UNKNOWN;
    return XAOT_ENCODING_UNPROVEN_NONE;
}

static uint32_t encoding_evidence_for(const XgEncodingOpSummary *enc) {
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

static bool xaot_bundle_add_encoding_plan(XaotBundle *bundle, const XgEncodingOpSummary *enc) {
    XaotEncodingPlan *plan;
    if (!bundle || !enc)
        return false;
    if (!reserve_plan_array((void **) &bundle->encoding_plans, &bundle->encoding_plan_cap,
                            bundle->nencoding_plans + 1, sizeof(XaotEncodingPlan), 8))
        return false;
    plan = &bundle->encoding_plans[bundle->nencoding_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->op_id = enc->op_id;
    plan->owner_func_id = enc->owner_func_id;
    plan->source_span_id = enc->source_span_id;
    plan->body_ordinal = enc->body_ordinal;
    plan->op_kind = enc->op_kind;
    plan->input_type_key = enc->input_type_key;
    plan->output_type_key = enc->output_type_key;
    plan->action = encoding_action_for(enc);
    plan->evidence = encoding_evidence_for(enc);
    plan->unproven_reason = encoding_reason_for(enc);
    return true;
}

static bool xaot_bundle_add_encoding_plans(XaotBundle *bundle, const XgGlobalEvidence *evidence) {
    if (!bundle || !evidence)
        return false;
    for (uint32_t i = 0; i < evidence->nencoding_ops; i++) {
        if (!xaot_bundle_add_encoding_plan(bundle, &evidence->encoding_ops[i]))
            return false;
    }
    return true;
}

static uint32_t xaot_metadata_profile_action(uint32_t profile, uint32_t metadata) {
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

static bool xaot_bundle_add_metadata_plan(XaotBundle *bundle, uint32_t metadata,
                                          uint32_t body_count, uint32_t decl_count) {
    XaotMetadataReachabilityPlan *plan;
    uint32_t evidence = 0;
    if (!bundle || metadata == 0 || (body_count == 0 && decl_count == 0))
        return true;
    if (!reserve_plan_array((void **) &bundle->metadata_plans, &bundle->metadata_plan_cap,
                            bundle->nmetadata_plans + 1, sizeof(XaotMetadataReachabilityPlan), 8))
        return false;
    if (body_count != 0)
        evidence |= XAOT_METADATA_EV_GLOBAL_BODY;
    if (decl_count != 0)
        evidence |= XAOT_METADATA_EV_DECL_ATTRIBUTE;
    plan = &bundle->metadata_plans[bundle->nmetadata_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->metadata = metadata;
    plan->body_count = body_count;
    plan->decl_count = decl_count;
    plan->evidence = evidence;
    plan->profile_action =
        xaot_metadata_profile_action(bundle->global_evidence_plan.profile, metadata);
    plan->unproven_reason = XAOT_METADATA_UNPROVEN_NONE;
    return true;
}

static bool xaot_bundle_add_metadata_plans(XaotBundle *bundle, const XgGlobalEvidence *evidence) {
    uint32_t metadata_count = 0;
    const uint32_t *metadata = xg_metadata_catalog(&metadata_count);
    for (uint32_t mi = 0; mi < metadata_count; mi++) {
        uint32_t bit = metadata[mi];
        uint32_t body_count = 0;
        uint32_t decl_count = 0;
        for (uint32_t bi = 0; bi < evidence->nbodies; bi++) {
            if ((evidence->bodies[bi].metadata_use_bits & bit) != 0)
                body_count++;
        }
        for (uint32_t di = 0; di < evidence->ndecls; di++) {
            if (bit == XG_METADATA_DERIVE && evidence->decls[di].derive_flags != 0)
                decl_count++;
        }
        if (!xaot_bundle_add_metadata_plan(bundle, bit, body_count, decl_count))
            return false;
    }
    return true;
}

static uint32_t xaot_capability_profile_action(uint32_t profile, uint32_t capability) {
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

static XaotCapabilityPlan *xaot_bundle_find_capability_plan_mut(XaotBundle *bundle,
                                                                uint32_t capability) {
    if (!bundle || capability == 0)
        return NULL;
    for (uint32_t i = 0; i < bundle->ncapability_plans; i++) {
        if (bundle->capability_plans[i].capability == capability)
            return &bundle->capability_plans[i];
    }
    return NULL;
}

static bool xaot_bundle_add_capability_plan(XaotBundle *bundle, uint32_t capability,
                                            uint32_t body_count, uint32_t transfer_count) {
    XaotCapabilityPlan *plan;
    uint32_t evidence = 0;
    if (!bundle || capability == 0 || (body_count == 0 && transfer_count == 0))
        return true;
    plan = xaot_bundle_find_capability_plan_mut(bundle, capability);
    if (!plan) {
        if (!reserve_plan_array((void **) &bundle->capability_plans, &bundle->capability_plan_cap,
                                bundle->ncapability_plans + 1, sizeof(XaotCapabilityPlan), 8))
            return false;
        plan = &bundle->capability_plans[bundle->ncapability_plans++];
        memset(plan, 0, sizeof(*plan));
    }
    if (body_count != 0)
        evidence |= XAOT_CAPABILITY_EV_GLOBAL_BODY;
    if (transfer_count != 0)
        evidence |= XAOT_CAPABILITY_EV_TRANSFER_PLAN;
    plan->capability = capability;
    plan->body_count = body_count;
    plan->transfer_count = transfer_count;
    plan->evidence = evidence;
    plan->profile_action =
        xaot_capability_profile_action(bundle->global_evidence_plan.profile, capability);
    plan->unproven_reason = XAOT_CAPABILITY_UNPROVEN_NONE;
    return true;
}

static bool xaot_bundle_add_capability_plans(XaotBundle *bundle, const XgGlobalEvidence *evidence) {
    uint32_t capability_count = 0;
    const uint32_t *capabilities = xg_capability_catalog(&capability_count);
    for (uint32_t ci = 0; ci < capability_count; ci++) {
        uint32_t cap = capabilities[ci];
        uint32_t body_count = 0;
        for (uint32_t bi = 0; bi < evidence->nbodies; bi++) {
            if ((evidence->bodies[bi].capability_bits & cap) != 0)
                body_count++;
        }
        if (!xaot_bundle_add_capability_plan(bundle, cap, body_count, 0))
            return false;
    }
    return true;
}

static uint64_t xaot_static_data_hash_mix_u32(uint64_t hash, uint32_t value) {
    hash ^= (uint64_t) value;
    return hash * UINT64_C(1099511628211);
}

static uint64_t xaot_static_data_hash_mix_u64(uint64_t hash, uint64_t value) {
    hash = xaot_static_data_hash_mix_u32(hash, (uint32_t) value);
    return xaot_static_data_hash_mix_u32(hash, (uint32_t) (value >> 32));
}

XR_FUNC uint32_t xaot_static_data_action_for(uint32_t profile, uint32_t static_data) {
    if (profile == XG_BUILD_FREESTANDING && static_data == XG_STATIC_DATA_RUNTIME_INIT)
        return XAOT_STATIC_DATA_ACTION_REJECT;
    if (static_data == XG_STATIC_DATA_RUNTIME_INIT)
        return XAOT_STATIC_DATA_ACTION_RUNTIME_INIT;
    if (static_data == XG_STATIC_DATA_FREESTANDING_SAFE)
        return XAOT_STATIC_DATA_ACTION_PROVE;
    return XAOT_STATIC_DATA_ACTION_MATERIALIZE;
}

XR_FUNC uint32_t xaot_static_data_section_for(uint32_t static_data, uint32_t action) {
    switch ((XaotStaticDataAction) action) {
        case XAOT_STATIC_DATA_ACTION_PROVE:
            return XAOT_STATIC_DATA_SECTION_EVIDENCE;
        case XAOT_STATIC_DATA_ACTION_MATERIALIZE:
            (void) static_data;
            return XAOT_STATIC_DATA_SECTION_RODATA;
        case XAOT_STATIC_DATA_ACTION_RUNTIME_INIT:
            return XAOT_STATIC_DATA_SECTION_RUNTIME_INIT;
        case XAOT_STATIC_DATA_ACTION_REJECT:
        default:
            return XAOT_STATIC_DATA_SECTION_NONE;
    }
}

XR_FUNC uint32_t xaot_static_data_align_for(uint32_t static_data, uint32_t action) {
    if (action != XAOT_STATIC_DATA_ACTION_MATERIALIZE)
        return 0;
    if (static_data == XG_STATIC_DATA_RODATA)
        return 1;
    return 8;
}

XR_FUNC uint64_t xaot_static_data_type_hash_for(uint32_t static_data, uint32_t action) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = xaot_static_data_hash_mix_u32(hash, UINT32_C(0x58534454)); /* XSDT */
    hash = xaot_static_data_hash_mix_u32(hash, static_data);
    hash = xaot_static_data_hash_mix_u32(hash, action);
    hash = xaot_static_data_hash_mix_u32(hash, xaot_static_data_section_for(static_data, action));
    return xaot_static_data_hash_mix_u32(hash, xaot_static_data_align_for(static_data, action));
}

XR_FUNC uint64_t xaot_static_data_data_hash_for(const XgGlobalEvidence *evidence,
                                                uint32_t static_data, uint32_t action) {
    uint64_t hash = UINT64_C(1469598103934665603);
    uint32_t matched = 0;

    if (!evidence || static_data == 0)
        return 0;

    hash = xaot_static_data_hash_mix_u32(hash, UINT32_C(0x58534444)); /* XSDD */
    hash = xaot_static_data_hash_mix_u32(hash, static_data);
    hash = xaot_static_data_hash_mix_u32(hash, action);
    for (uint32_t bi = 0; bi < evidence->nbodies; bi++) {
        const XgBodySummary *body = &evidence->bodies[bi];
        if ((body->static_data_use_bits & static_data) == 0)
            continue;
        matched++;
        hash = xaot_static_data_hash_mix_u32(hash, body->func_id);
        hash = xaot_static_data_hash_mix_u32(hash, body->module_id);
        hash = xaot_static_data_hash_mix_u32(hash, body->source_span_id);
        hash = xaot_static_data_hash_mix_u64(hash, body->body_hash);
        hash = xaot_static_data_hash_mix_u32(hash, body->static_data_use_bits);
    }
    if (matched == 0)
        return 0;
    return xaot_static_data_hash_mix_u32(hash, matched);
}

static bool xaot_bundle_add_static_data_plan(XaotBundle *bundle, uint32_t static_data,
                                             const XgGlobalEvidence *evidence,
                                             uint32_t body_count) {
    XaotStaticDataPlan *plan;
    if (!bundle || static_data == 0 || body_count == 0)
        return true;
    if (!reserve_plan_array((void **) &bundle->static_data_plans, &bundle->static_data_plan_cap,
                            bundle->nstatic_data_plans + 1, sizeof(XaotStaticDataPlan), 8))
        return false;
    plan = &bundle->static_data_plans[bundle->nstatic_data_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->static_data = static_data;
    plan->body_count = body_count;
    plan->evidence = XAOT_STATIC_DATA_EV_GLOBAL_BODY;
    plan->action = xaot_static_data_action_for(bundle->global_evidence_plan.profile, static_data);
    plan->section = xaot_static_data_section_for(static_data, plan->action);
    plan->align = xaot_static_data_align_for(static_data, plan->action);
    plan->type_hash = xaot_static_data_type_hash_for(static_data, plan->action);
    plan->data_hash = xaot_static_data_data_hash_for(evidence, static_data, plan->action);
    plan->unproven_reason = XAOT_STATIC_DATA_UNPROVEN_NONE;
    return true;
}

static bool xaot_bundle_add_static_data_plans(XaotBundle *bundle,
                                              const XgGlobalEvidence *evidence) {
    uint32_t static_data_count = 0;
    const uint32_t *static_data = xg_static_data_catalog(&static_data_count);
    for (uint32_t si = 0; si < static_data_count; si++) {
        uint32_t bit = static_data[si];
        uint32_t body_count = 0;
        for (uint32_t bi = 0; bi < evidence->nbodies; bi++) {
            if ((evidence->bodies[bi].static_data_use_bits & bit) != 0)
                body_count++;
        }
        if (!xaot_bundle_add_static_data_plan(bundle, bit, evidence, body_count))
            return false;
    }
    return true;
}

static bool xaot_bundle_add_link_dependency_plan(XaotBundle *bundle,
                                                 const XgLinkDependencySummary *summary) {
    XaotLinkDependencyPlan *plan;
    if (!bundle || !summary || summary->link_id == XG_NO_ID || summary->kind == 0 ||
        !summary->name[0])
        return true;
    if (!reserve_plan_array((void **) &bundle->link_dependency_plans,
                            &bundle->link_dependency_plan_cap, bundle->nlink_dependency_plans + 1,
                            sizeof(XaotLinkDependencyPlan), 8))
        return false;
    plan = &bundle->link_dependency_plans[bundle->nlink_dependency_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->link_id = summary->link_id;
    plan->kind = summary->kind;
    plan->name_id = summary->name_id;
    plan->evidence = XAOT_LINK_DEP_EV_GLOBAL_SUMMARY;
    plan->unproven_reason = XAOT_LINK_DEP_UNPROVEN_NONE;
    memcpy(plan->name, summary->name, sizeof(plan->name));
    plan->name[XG_LINK_DEP_NAME_MAX - 1] = '\0';
    return true;
}

static bool xaot_bundle_add_link_dependency_plans(XaotBundle *bundle,
                                                  const XgGlobalEvidence *evidence) {
    if (!bundle || !evidence)
        return false;
    for (uint32_t i = 0; i < evidence->nlink_deps; i++) {
        if (!xaot_bundle_add_link_dependency_plan(bundle, &evidence->link_deps[i]))
            return false;
    }
    return true;
}

static bool xaot_bundle_populate_global_lowered_plans(XaotBundle *bundle,
                                                      const XgGlobalEvidence *evidence) {
    if (!bundle || !evidence)
        return false;
    for (uint32_t i = 0; i < evidence->nclasses; i++) {
        const XgClassSummary *summary = &evidence->classes[i];
        if (!xg_class_summary_is_runtime_class(summary))
            continue;
        if (!xaot_bundle_add_class_hierarchy_plan(bundle, summary) ||
            !xaot_bundle_add_class_layout_plan(bundle, summary)) {
            bundle->error_msg = "failed to allocate AOT class plan";
            return false;
        }
    }
    for (uint32_t i = 0; i < evidence->ncallsites; i++) {
        const XgCallsiteSummary *call = &evidence->callsites[i];
        if (call->kind != XG_CALL_METHOD && call->kind != XG_CALL_INTERFACE)
            continue;
        if (!xaot_bundle_add_method_dispatch_plan(bundle, call, evidence)) {
            bundle->error_msg = "failed to allocate AOT method dispatch plan";
            return false;
        }
    }
    for (uint32_t i = 0; i < evidence->ninterface_impls; i++) {
        if (!xaot_bundle_add_interface_use_plan(bundle, &evidence->interface_impls[i])) {
            bundle->error_msg = "failed to allocate AOT interface use plan";
            return false;
        }
    }
    for (uint32_t i = 0; i < evidence->ninterface_object_uses; i++) {
        if (!xaot_bundle_add_interface_object_use_plan(bundle, evidence,
                                                       &evidence->interface_object_uses[i])) {
            bundle->error_msg = "failed to allocate AOT interface object use plan";
            return false;
        }
    }
    if (!xaot_bundle_add_interface_abi_plans(bundle, evidence)) {
        bundle->error_msg = "failed to allocate AOT interface ABI plan";
        return false;
    }
    if (!xaot_bundle_add_generic_specialization_plans(bundle, evidence)) {
        bundle->error_msg = "failed to allocate AOT generic specialization plan";
        return false;
    }
    if (!xaot_bundle_add_generic_instantiation_plans(bundle, evidence)) {
        bundle->error_msg = "failed to allocate AOT generic instantiation plan";
        return false;
    }
    if (!xaot_bundle_add_generic_body_plans(bundle, evidence)) {
        bundle->error_msg = "failed to allocate AOT generic body plan";
        return false;
    }
    if (!xaot_bundle_add_generic_storage_plans(bundle, evidence)) {
        bundle->error_msg = "failed to allocate AOT generic storage plan";
        return false;
    }
    if (!xaot_bundle_add_generic_code_size_plans(bundle, evidence)) {
        bundle->error_msg = "failed to allocate AOT generic code-size plan";
        return false;
    }
    if (!xaot_bundle_add_derive_plans(bundle, evidence)) {
        bundle->error_msg = "failed to allocate AOT derive plan";
        return false;
    }
    if (!xaot_bundle_add_derived_eq_hash_plans(bundle, evidence)) {
        bundle->error_msg = "failed to allocate AOT derived Eq/Hash plan";
        return false;
    }
    if (!xaot_bundle_add_derived_clone_plans(bundle, evidence)) {
        bundle->error_msg = "failed to allocate AOT derived Clone plan";
        return false;
    }
    if (!xaot_bundle_add_json_shape_plans(bundle, evidence)) {
        bundle->error_msg = "failed to allocate AOT Json shape plan";
        return false;
    }
    if (!xaot_bundle_add_json_access_plans(bundle, evidence)) {
        bundle->error_msg = "failed to allocate AOT Json access plan";
        return false;
    }
    if (!xaot_bundle_add_json_codec_plans(bundle, evidence)) {
        bundle->error_msg = "failed to allocate AOT Json codec plan";
        return false;
    }
    if (!xaot_bundle_add_record_shape_plans(bundle, evidence)) {
        bundle->error_msg = "failed to allocate AOT Record shape plan";
        return false;
    }
    if (!xaot_bundle_add_record_access_plans(bundle, evidence)) {
        bundle->error_msg = "failed to allocate AOT Record access plan";
        return false;
    }
    if (!xaot_bundle_add_options_plans(bundle, evidence)) {
        bundle->error_msg = "failed to allocate AOT options plan";
        return false;
    }
    if (!xaot_bundle_add_map_shape_plans(bundle, evidence)) {
        bundle->error_msg = "failed to allocate AOT Map/Set shape plan";
        return false;
    }
    if (!xaot_bundle_add_hash_eq_plans(bundle, evidence)) {
        bundle->error_msg = "failed to allocate AOT Hash/Eq plan";
        return false;
    }
    if (!xaot_bundle_add_key_access_plans(bundle, evidence)) {
        bundle->error_msg = "failed to allocate AOT Map/Set key access plan";
        return false;
    }
    if (!xaot_bundle_add_sequence_access_plans(bundle, evidence)) {
        bundle->error_msg = "failed to allocate AOT sequence access plan";
        return false;
    }
    if (!xaot_bundle_add_capacity_plans(bundle, evidence)) {
        bundle->error_msg = "failed to allocate AOT capacity plan";
        return false;
    }
    if (!xaot_bundle_add_bulk_plans(bundle, evidence)) {
        bundle->error_msg = "failed to allocate AOT bulk plan";
        return false;
    }
    if (!xaot_bundle_add_encoding_plans(bundle, evidence)) {
        bundle->error_msg = "failed to allocate AOT encoding plan";
        return false;
    }
    if (!xaot_bundle_add_metadata_plans(bundle, evidence)) {
        bundle->error_msg = "failed to allocate AOT metadata plan";
        return false;
    }
    if (!xaot_bundle_add_capability_plans(bundle, evidence)) {
        bundle->error_msg = "failed to allocate AOT capability plan";
        return false;
    }
    if (!xaot_bundle_add_static_data_plans(bundle, evidence)) {
        bundle->error_msg = "failed to allocate AOT static data plan";
        return false;
    }
    if (!xaot_bundle_add_link_dependency_plans(bundle, evidence)) {
        bundle->error_msg = "failed to allocate AOT link dependency plan";
        return false;
    }
    return true;
}

static const XiFunc *xaot_bundle_find_method_func_in_module(const XiModule *module,
                                                            const XgClassSummary *class_summary,
                                                            const XgMethodSummary *method_summary);

static bool xaot_bundle_module_matches_evidence_id(uint32_t evidence_module_id,
                                                   uint32_t module_index) {
    return evidence_module_id == XG_NO_ID || evidence_module_id == module_index + 1;
}

static void xaot_bundle_bind_xi_func_body_id(XiFunc *func, XgFuncId body_func_id) {
    if (!func || body_func_id == XG_NO_ID)
        return;
    if (func->xg_body_func_id == XG_NO_ID)
        func->xg_body_func_id = body_func_id;
}

static void xaot_bundle_bind_method_body_func_ids(XaotBundle *bundle) {
    const XgGlobalEvidence *ev;
    if (!bundle || !bundle->modules)
        return;
    ev = bundle->global_evidence_plan.evidence;
    if (!ev)
        return;
    for (uint32_t bi = 0; bi < ev->nbodies; bi++) {
        const XgBodySummary *body = &ev->bodies[bi];
        const XgMethodSummary *method;
        const XgClassSummary *class_summary;
        XiFunc *match = NULL;
        if (body->kind != XG_BODY_METHOD || body->func_id == XG_NO_ID ||
            body->owner_method_id == XG_NO_ID)
            continue;
        method = xg_evidence_find_method_by_id(ev, body->owner_method_id);
        class_summary = method ? xg_evidence_find_class(ev, method->owner_class_id) : NULL;
        if (!method || !class_summary)
            continue;
        if (body->owner_class_id != XG_NO_ID && body->owner_class_id != method->owner_class_id)
            continue;
        if (body->name_id != 0 && body->name_id != method->name_id)
            continue;
        if (body->signature_key != 0 && body->signature_key != method->signature_key)
            continue;
        for (uint32_t mi = 0; mi < bundle->nmodules; mi++) {
            XiModule *module = bundle->modules[mi];
            XiFunc *func;
            if (!module)
                continue;
            if (!xaot_bundle_module_matches_evidence_id(body->module_id, mi))
                continue;
            if (!xaot_bundle_module_matches_evidence_id(class_summary->module_id, mi))
                continue;
            func = (XiFunc *) xaot_bundle_find_method_func_in_module(module, class_summary, method);
            if (!func)
                continue;
            if (match && match != func) {
                match = NULL;
                break;
            }
            match = func;
        }
        xaot_bundle_bind_xi_func_body_id(match, body->func_id);
    }
}

static XgFuncId xaot_bundle_find_unique_body_func_id_for_xi_func(const XaotBundle *bundle,
                                                                 const XiFunc *func,
                                                                 uint32_t module_index,
                                                                 bool is_module_init) {
    const XgGlobalEvidence *ev;
    XgFuncId match = XG_NO_ID;
    uint32_t name_id = 0;
    if (!bundle || !func)
        return XG_NO_ID;
    ev = bundle->global_evidence_plan.evidence;
    if (!ev)
        return XG_NO_ID;
    if (!is_module_init) {
        name_id = xg_name_id(func->name);
        if (name_id == 0)
            return XG_NO_ID;
    }
    for (uint32_t i = 0; i < ev->nbodies; i++) {
        const XgBodySummary *body = &ev->bodies[i];
        if (body->func_id == XG_NO_ID)
            continue;
        if (!xaot_bundle_module_matches_evidence_id(body->module_id, module_index))
            continue;
        if (is_module_init) {
            if (body->kind != XG_BODY_MODULE_INIT)
                continue;
        } else {
            if (body->kind != XG_BODY_FUNCTION || body->name_id != name_id)
                continue;
        }
        if (match != XG_NO_ID)
            return XG_NO_ID;
        match = body->func_id;
    }
    return match;
}

static void xaot_bundle_bind_body_func_ids_in_func(XaotBundle *bundle, XiFunc *func,
                                                   uint32_t module_index, bool is_module_init) {
    if (!bundle || !func)
        return;
    if (func->xg_body_func_id == XG_NO_ID)
        func->xg_body_func_id = xaot_bundle_find_unique_body_func_id_for_xi_func(
            bundle, func, module_index, is_module_init);
    for (uint16_t ci = 0; ci < func->nchildren; ci++)
        xaot_bundle_bind_body_func_ids_in_func(bundle, func->children ? func->children[ci] : NULL,
                                               module_index, false);
}

static void xaot_bundle_bind_xi_body_func_ids(XaotBundle *bundle) {
    if (!bundle || !bundle->modules)
        return;
    xaot_bundle_bind_method_body_func_ids(bundle);
    for (uint32_t mi = 0; mi < bundle->nmodules; mi++) {
        XiModule *module = bundle->modules[mi];
        if (!module)
            continue;
        xaot_bundle_bind_body_func_ids_in_func(bundle, module->init, mi, true);
        for (uint16_t fi = 0; fi < module->nfuncs; fi++)
            xaot_bundle_bind_body_func_ids_in_func(
                bundle, module->functions ? module->functions[fi] : NULL, mi, false);
    }
}

static void xaot_bundle_bind_callsite_ids_in_func(XaotBundle *bundle, XiFunc *func) {
    if (!bundle || !func)
        return;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        XiBlock *blk = func->blocks ? func->blocks[bi] : NULL;
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *value = blk->values ? blk->values[vi] : NULL;
            const XaotMethodDispatchPlan *plan;
            if (!value || (value->op != XI_CALL_METHOD && value->op != XI_CALL_METHOD_DIRECT) ||
                value->xg_callsite_id != XG_NO_ID)
                continue;
            plan = xaot_bundle_find_method_dispatch_plan_for_xi_call(bundle, value);
            if (plan) {
                value->xg_callsite_id = plan->callsite_id;
                value->xg_method_id = plan->method_id;
            }
        }
    }
    for (uint16_t ci = 0; ci < func->nchildren; ci++)
        xaot_bundle_bind_callsite_ids_in_func(bundle, func->children ? func->children[ci] : NULL);
}

static void xaot_bundle_bind_xi_callsite_ids(XaotBundle *bundle) {
    if (!bundle || !bundle->modules)
        return;
    for (uint32_t mi = 0; mi < bundle->nmodules; mi++) {
        XiModule *module = bundle->modules[mi];
        if (!module)
            continue;
        xaot_bundle_bind_callsite_ids_in_func(bundle, module->init);
        for (uint16_t fi = 0; fi < module->nfuncs; fi++)
            xaot_bundle_bind_callsite_ids_in_func(bundle,
                                                  module->functions ? module->functions[fi] : NULL);
    }
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
    xr_free(bundle->allocation_plans);
    xr_free(bundle->closure_plans);
    xr_free(bundle->transfer_plans);
    xaot_bundle_clear_global_lowered_plans(bundle);
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
    xaot_ptr_index_free(&bundle->allocation_index);
    xaot_ptr_index_free(&bundle->closure_index);
    memset(bundle, 0, sizeof(*bundle));
}

XR_FUNC bool xaot_bundle_set_global_evidence(XaotBundle *bundle, const XgGlobalEvidence *evidence,
                                             uint32_t profile) {
    if (!bundle || !evidence)
        return false;
    xaot_bundle_clear_global_lowered_plans(bundle);
    bundle->global_evidence_plan.evidence = evidence;
    bundle->global_evidence_plan.evidence_hash = xg_global_evidence_hash(evidence);
    bundle->global_evidence_plan.profile = profile;
    if (!xaot_bundle_populate_global_lowered_plans(bundle, evidence))
        return false;
    xaot_bundle_bind_xi_body_func_ids(bundle);
    xaot_bundle_bind_xi_callsite_ids(bundle);
    return true;
}

XR_FUNC const XaotClassHierarchyPlan *
xaot_bundle_find_class_hierarchy_plan(const XaotBundle *bundle, XgClassId class_id) {
    if (!bundle || class_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < bundle->nclass_hierarchy_plans; i++) {
        if (bundle->class_hierarchy_plans[i].class_id == class_id)
            return &bundle->class_hierarchy_plans[i];
    }
    return NULL;
}

XR_FUNC const XaotClassLayoutPlan *xaot_bundle_find_class_layout_plan(const XaotBundle *bundle,
                                                                      XgClassId class_id) {
    if (!bundle || class_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < bundle->nclass_layout_plans; i++) {
        if (bundle->class_layout_plans[i].class_id == class_id)
            return &bundle->class_layout_plans[i];
    }
    return NULL;
}

XR_FUNC const XaotMethodDispatchPlan *
xaot_bundle_find_method_dispatch_plan(const XaotBundle *bundle, XgCallsiteId callsite_id) {
    if (!bundle || callsite_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < bundle->nmethod_dispatch_plans; i++) {
        if (bundle->method_dispatch_plans[i].callsite_id == callsite_id)
            return &bundle->method_dispatch_plans[i];
    }
    return NULL;
}

static XgFuncId xi_call_owner_body_func_id(const XiValue *call) {
    if (!call || !call->block || !call->block->func)
        return XG_NO_ID;
    return (XgFuncId) call->block->func->xg_body_func_id;
}

XR_FUNC const XaotMethodDispatchPlan *
xaot_bundle_find_method_dispatch_plan_for_xi_call(const XaotBundle *bundle, const XiValue *call) {
    const XaotMethodDispatchPlan *match = NULL;
    const char *method_name;
    uint32_t method_name_id;
    uint16_t arg_count;
    XgFuncId owner_func_id;
    if (!bundle || !call || (call->op != XI_CALL_METHOD && call->op != XI_CALL_METHOD_DIRECT) ||
        call->nargs == 0)
        return NULL;
    method_name = call->aux ? (const char *) call->aux : NULL;
    method_name_id = xg_name_id(method_name);
    if (method_name_id == 0)
        return NULL;
    arg_count = (uint16_t) (call->nargs - 1);
    owner_func_id = xi_call_owner_body_func_id(call);
    if (call->xg_callsite_id != XG_NO_ID) {
        const XaotMethodDispatchPlan *plan =
            xaot_bundle_find_method_dispatch_plan(bundle, call->xg_callsite_id);
        if (!plan)
            return NULL;
        if (plan->callsite_id != call->xg_callsite_id)
            return NULL;
        if (call->xg_method_id != XG_NO_ID && plan->method_id != call->xg_method_id)
            return NULL;
        if (owner_func_id != XG_NO_ID && plan->owner_func_id != owner_func_id)
            return NULL;
        if (call->line != 0 && plan->source_span_id != call->line)
            return NULL;
        if (plan->arg_count != arg_count)
            return NULL;
        if (plan->method_name_id == 0 || plan->method_name_id != method_name_id)
            return NULL;
        return plan;
    }
    if (call->line == 0)
        return NULL;
    for (uint32_t i = 0; i < bundle->nmethod_dispatch_plans; i++) {
        const XaotMethodDispatchPlan *plan = &bundle->method_dispatch_plans[i];
        if (owner_func_id != XG_NO_ID && plan->owner_func_id != owner_func_id)
            continue;
        if (plan->source_span_id != call->line)
            continue;
        if (plan->arg_count != arg_count)
            continue;
        if (call->xg_method_id != XG_NO_ID && plan->method_id != call->xg_method_id)
            continue;
        if (plan->method_name_id == 0 || plan->method_name_id != method_name_id)
            continue;
        if (match)
            return NULL;
        match = plan;
    }
    return match;
}

static bool xi_class_data_name_matches_id(const XiClassData *class_data, uint32_t name_id) {
    if (!class_data || name_id == 0)
        return false;
    if (xg_name_id(class_data->class_name) == name_id)
        return true;
    if (xg_name_id(class_data->display_name) == name_id)
        return true;
    return xg_name_id(class_data->generic_origin_name) == name_id;
}

static const XiFunc *xaot_bundle_find_method_func_in_module(const XiModule *module,
                                                            const XgClassSummary *class_summary,
                                                            const XgMethodSummary *method_summary) {
    const XiFunc *match = NULL;
    bool want_static;
    if (!module || !module->init || !class_summary || !method_summary ||
        class_summary->name_id == 0 || method_summary->name_id == 0)
        return NULL;
    want_static = (method_summary->flags & XG_METHOD_STATIC) != 0;
    for (uint16_t ci = 0; ci < module->nclasses; ci++) {
        const XiClassData *class_data = module->classes ? module->classes[ci] : NULL;
        if (!xi_class_data_name_matches_id(class_data, class_summary->name_id) ||
            !class_data->methods || !class_data->child_idx)
            continue;
        for (uint16_t mi = 0; mi < class_data->nmethod; mi++) {
            const XiClassMethod *method = &class_data->methods[mi];
            uint16_t child_idx;
            const XiFunc *func;
            if (method->is_static_constructor || method->is_static != want_static ||
                xg_name_id(method->name) != method_summary->name_id ||
                mi >= class_data->ninst + class_data->nstat)
                continue;
            child_idx = class_data->child_idx[mi];
            if (child_idx >= module->init->nchildren)
                continue;
            func = module->init->children[child_idx];
            if (!func)
                continue;
            if (match && match != func)
                return NULL;
            match = func;
        }
    }
    return match;
}

XR_FUNC const XiFunc *xaot_bundle_find_method_func(const XaotBundle *bundle, XgMethodId method_id,
                                                   const char **out_module_prefix) {
    const XgGlobalEvidence *evidence;
    const XgMethodSummary *method;
    const XgClassSummary *class_summary;
    const XiFunc *match = NULL;
    const char *match_prefix = NULL;
    if (out_module_prefix)
        *out_module_prefix = NULL;
    if (!bundle || method_id == XG_NO_ID)
        return NULL;
    evidence = bundle->global_evidence_plan.evidence;
    method = xg_evidence_find_method_by_id(evidence, method_id);
    class_summary = method ? xg_evidence_find_class(evidence, method->owner_class_id) : NULL;
    if (!method || !class_summary)
        return NULL;

    for (uint32_t mi = 0; mi < bundle->nmodules; mi++) {
        const XiModule *module = bundle->modules ? bundle->modules[mi] : NULL;
        const XiFunc *func;
        if (class_summary->module_id != 0 && class_summary->module_id != mi + 1)
            continue;
        func = xaot_bundle_find_method_func_in_module(module, class_summary, method);
        if (!func)
            continue;
        if (match && match != func)
            return NULL;
        match = func;
        match_prefix = module ? module->name : NULL;
    }

    if (match && out_module_prefix)
        *out_module_prefix = match_prefix;
    return match;
}

static bool xaot_collect_body_func_match(const XiFunc *func, XgFuncId body_func_id,
                                         const XiFunc **match) {
    if (!func || !match)
        return true;
    if ((XgFuncId) func->xg_body_func_id == body_func_id) {
        if (*match && *match != func)
            return false;
        *match = func;
    }
    for (uint16_t ci = 0; ci < func->nchildren; ci++) {
        if (!xaot_collect_body_func_match(func->children ? func->children[ci] : NULL, body_func_id,
                                          match))
            return false;
    }
    return true;
}

XR_FUNC const XiFunc *xaot_bundle_find_body_func(const XaotBundle *bundle, XgFuncId body_func_id,
                                                 const char **out_module_prefix) {
    const XiFunc *match = NULL;
    const char *match_prefix = NULL;
    if (out_module_prefix)
        *out_module_prefix = NULL;
    if (!bundle || body_func_id == XG_NO_ID)
        return NULL;
    for (uint32_t mi = 0; mi < bundle->nmodules; mi++) {
        XiModule *module = bundle->modules ? bundle->modules[mi] : NULL;
        const XiFunc *module_match = NULL;
        if (!module)
            continue;
        if (!xaot_collect_body_func_match(module->init, body_func_id, &module_match))
            return NULL;
        for (uint16_t fi = 0; fi < module->nfuncs; fi++) {
            if (!xaot_collect_body_func_match(module->functions ? module->functions[fi] : NULL,
                                              body_func_id, &module_match))
                return NULL;
        }
        if (!module_match)
            continue;
        if (match && match != module_match)
            return NULL;
        match = module_match;
        match_prefix = module->name;
    }
    if (match && out_module_prefix)
        *out_module_prefix = match_prefix;
    return match;
}

XR_FUNC const XiFunc *xaot_bundle_find_dispatch_target_func(const XaotBundle *bundle,
                                                            const XaotDispatchTargetCase *target,
                                                            const char **out_module_prefix) {
    if (out_module_prefix)
        *out_module_prefix = NULL;
    if (!target)
        return NULL;
    if (target->method_body_func_id != XG_NO_ID)
        return xaot_bundle_find_body_func(bundle, target->method_body_func_id, out_module_prefix);
    return xaot_bundle_find_method_func(bundle, target->method_id, out_module_prefix);
}

XR_FUNC const XaotInterfaceUsePlan *
xaot_bundle_find_interface_use_plan(const XaotBundle *bundle, XgInterfaceId interface_id,
                                    XgClassId implementor_class_id, XgCallsiteId use_site_id) {
    if (!bundle || interface_id == XG_NO_ID || implementor_class_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < bundle->ninterface_use_plans; i++) {
        const XaotInterfaceUsePlan *plan = &bundle->interface_use_plans[i];
        if (plan->interface_id == interface_id &&
            plan->implementor_class_id == implementor_class_id && plan->use_site_id == use_site_id)
            return plan;
    }
    return NULL;
}

XR_FUNC const XaotInterfaceAbiPlan *
xaot_bundle_find_interface_abi_plan(const XaotBundle *bundle, XgInterfaceId interface_id) {
    if (!bundle || interface_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < bundle->ninterface_abi_plans; i++) {
        if (bundle->interface_abi_plans[i].interface_id == interface_id)
            return &bundle->interface_abi_plans[i];
    }
    return NULL;
}

XR_FUNC const XaotGenericSpecializationPlan *
xaot_bundle_find_generic_specialization_plan(const XaotBundle *bundle, XgCallsiteId callsite_id) {
    if (!bundle || callsite_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < bundle->ngeneric_specialization_plans; i++) {
        if (bundle->generic_specialization_plans[i].callsite_id == callsite_id)
            return &bundle->generic_specialization_plans[i];
    }
    return NULL;
}

XR_FUNC const XaotGenericInstantiationPlan *
xaot_bundle_find_generic_instantiation_plan(const XaotBundle *bundle,
                                            XgGenericInstId generic_inst_id) {
    if (!bundle || generic_inst_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < bundle->ngeneric_instantiation_plans; i++) {
        if (bundle->generic_instantiation_plans[i].generic_inst_id == generic_inst_id)
            return &bundle->generic_instantiation_plans[i];
    }
    return NULL;
}

XR_FUNC const XaotGenericBodyPlan *xaot_bundle_find_generic_body_plan(const XaotBundle *bundle,
                                                                      XgGenericBodyUseId use_id) {
    if (!bundle || use_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < bundle->ngeneric_body_plans; i++) {
        if (bundle->generic_body_plans[i].use_id == use_id)
            return &bundle->generic_body_plans[i];
    }
    return NULL;
}

XR_FUNC const XaotGenericStoragePlan *
xaot_bundle_find_generic_storage_plan(const XaotBundle *bundle, XgGenericStorageId storage_id) {
    if (!bundle || storage_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < bundle->ngeneric_storage_plans; i++) {
        if (bundle->generic_storage_plans[i].storage_id == storage_id)
            return &bundle->generic_storage_plans[i];
    }
    return NULL;
}

XR_FUNC const XaotGenericCodeSizePlan *
xaot_bundle_find_generic_code_size_plan(const XaotBundle *bundle,
                                        XgGenericCodeSizeId code_size_id) {
    if (!bundle || code_size_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < bundle->ngeneric_code_size_plans; i++) {
        if (bundle->generic_code_size_plans[i].code_size_id == code_size_id)
            return &bundle->generic_code_size_plans[i];
    }
    return NULL;
}

XR_FUNC const XaotDerivePlan *xaot_bundle_find_derive_plan(const XaotBundle *bundle,
                                                           XgDeriveId derive_id) {
    if (!bundle || derive_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < bundle->nderive_plans; i++) {
        if (bundle->derive_plans[i].derive_id == derive_id)
            return &bundle->derive_plans[i];
    }
    return NULL;
}

XR_FUNC const XaotDerivedEqHashPlan *xaot_bundle_find_derived_eq_hash_plan(const XaotBundle *bundle,
                                                                           uint32_t type_key) {
    if (!bundle || type_key == 0)
        return NULL;
    for (uint32_t i = 0; i < bundle->nderived_eq_hash_plans; i++) {
        if (bundle->derived_eq_hash_plans[i].type_key == type_key)
            return &bundle->derived_eq_hash_plans[i];
    }
    return NULL;
}

XR_FUNC const XaotDerivedClonePlan *xaot_bundle_find_derived_clone_plan(const XaotBundle *bundle,
                                                                        uint32_t type_key) {
    if (!bundle || type_key == 0)
        return NULL;
    for (uint32_t i = 0; i < bundle->nderived_clone_plans; i++) {
        if (bundle->derived_clone_plans[i].type_key == type_key)
            return &bundle->derived_clone_plans[i];
    }
    return NULL;
}

XR_FUNC const XaotJsonShapePlan *xaot_bundle_find_json_shape_plan(const XaotBundle *bundle,
                                                                  XgJsonShapeId json_shape_id) {
    if (!bundle || json_shape_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < bundle->njson_shape_plans; i++) {
        if (bundle->json_shape_plans[i].json_shape_id == json_shape_id)
            return &bundle->json_shape_plans[i];
    }
    return NULL;
}

XR_FUNC const XaotJsonAccessPlan *xaot_bundle_find_json_access_plan(const XaotBundle *bundle,
                                                                    XgJsonAccessId json_access_id) {
    if (!bundle || json_access_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < bundle->njson_access_plans; i++) {
        if (bundle->json_access_plans[i].json_access_id == json_access_id)
            return &bundle->json_access_plans[i];
    }
    return NULL;
}

XR_FUNC const XaotJsonCodecPlan *xaot_bundle_find_json_codec_plan(const XaotBundle *bundle,
                                                                  XgJsonCodecId codec_id) {
    if (!bundle || codec_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < bundle->njson_codec_plans; i++) {
        if (bundle->json_codec_plans[i].codec_id == codec_id)
            return &bundle->json_codec_plans[i];
    }
    return NULL;
}

XR_FUNC const XaotRecordShapePlan *
xaot_bundle_find_record_shape_plan(const XaotBundle *bundle, XgRecordShapeId record_shape_id) {
    if (!bundle || record_shape_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < bundle->nrecord_shape_plans; i++) {
        if (bundle->record_shape_plans[i].record_shape_id == record_shape_id)
            return &bundle->record_shape_plans[i];
    }
    return NULL;
}

XR_FUNC const XaotRecordAccessPlan *
xaot_bundle_find_record_access_plan(const XaotBundle *bundle, XgRecordAccessId record_access_id) {
    if (!bundle || record_access_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < bundle->nrecord_access_plans; i++) {
        if (bundle->record_access_plans[i].record_access_id == record_access_id)
            return &bundle->record_access_plans[i];
    }
    return NULL;
}

XR_FUNC const XaotOptionsPlan *xaot_bundle_find_options_plan(const XaotBundle *bundle,
                                                             XgOptionsId options_id) {
    if (!bundle || options_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < bundle->noptions_plans; i++) {
        if (bundle->options_plans[i].options_id == options_id)
            return &bundle->options_plans[i];
    }
    return NULL;
}

XR_FUNC const XaotMapShapePlan *xaot_bundle_find_map_shape_plan(const XaotBundle *bundle,
                                                                XgMapShapeId shape_id) {
    if (!bundle || shape_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < bundle->nmap_shape_plans; i++) {
        if (bundle->map_shape_plans[i].shape_id == shape_id)
            return &bundle->map_shape_plans[i];
    }
    return NULL;
}

XR_FUNC const XaotKeyAccessPlan *xaot_bundle_find_key_access_plan(const XaotBundle *bundle,
                                                                  XgKeyAccessId access_id) {
    if (!bundle || access_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < bundle->nkey_access_plans; i++) {
        if (bundle->key_access_plans[i].access_id == access_id)
            return &bundle->key_access_plans[i];
    }
    return NULL;
}

XR_FUNC const XaotHashEqPlan *xaot_bundle_find_hash_eq_plan(const XaotBundle *bundle,
                                                            uint32_t type_key) {
    if (!bundle || type_key == 0)
        return NULL;
    for (uint32_t i = 0; i < bundle->nhash_eq_plans; i++) {
        if (bundle->hash_eq_plans[i].type_key == type_key)
            return &bundle->hash_eq_plans[i];
    }
    return NULL;
}

XR_FUNC const XaotSequenceAccessPlan *
xaot_bundle_find_sequence_access_plan(const XaotBundle *bundle, XgSequenceAccessId access_id) {
    if (!bundle || access_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < bundle->nsequence_access_plans; i++) {
        if (bundle->sequence_access_plans[i].access_id == access_id)
            return &bundle->sequence_access_plans[i];
    }
    return NULL;
}

XR_FUNC const XaotCapacityPlan *xaot_bundle_find_capacity_plan(const XaotBundle *bundle,
                                                               XgCapacityOpId op_id) {
    if (!bundle || op_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < bundle->ncapacity_plans; i++) {
        if (bundle->capacity_plans[i].op_id == op_id)
            return &bundle->capacity_plans[i];
    }
    return NULL;
}

XR_FUNC const XaotBulkPlan *xaot_bundle_find_bulk_plan(const XaotBundle *bundle, XgBulkOpId op_id) {
    if (!bundle || op_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < bundle->nbulk_plans; i++) {
        if (bundle->bulk_plans[i].op_id == op_id)
            return &bundle->bulk_plans[i];
    }
    return NULL;
}

XR_FUNC const XaotEncodingPlan *xaot_bundle_find_encoding_plan(const XaotBundle *bundle,
                                                               XgEncodingOpId op_id) {
    if (!bundle || op_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < bundle->nencoding_plans; i++) {
        if (bundle->encoding_plans[i].op_id == op_id)
            return &bundle->encoding_plans[i];
    }
    return NULL;
}

XR_FUNC const XaotMetadataReachabilityPlan *xaot_bundle_find_metadata_plan(const XaotBundle *bundle,
                                                                           uint32_t metadata) {
    if (!bundle || metadata == 0)
        return NULL;
    for (uint32_t i = 0; i < bundle->nmetadata_plans; i++) {
        if (bundle->metadata_plans[i].metadata == metadata)
            return &bundle->metadata_plans[i];
    }
    return NULL;
}

XR_FUNC const XaotCapabilityPlan *xaot_bundle_find_capability_plan(const XaotBundle *bundle,
                                                                   uint32_t capability) {
    if (!bundle || capability == 0)
        return NULL;
    for (uint32_t i = 0; i < bundle->ncapability_plans; i++) {
        if (bundle->capability_plans[i].capability == capability)
            return &bundle->capability_plans[i];
    }
    return NULL;
}

XR_FUNC bool xaot_bundle_sync_transfer_capability_plans(XaotBundle *bundle) {
    uint32_t deep_copy_transfer_count = 0;
    uint32_t body_count = 0;
    const XaotCapabilityPlan *existing;

    if (!bundle)
        return false;
    for (uint32_t i = 0; i < bundle->ntransfer_plans; i++) {
        if (bundle->transfer_plans[i].action == XAOT_TRANSFER_ACTION_DEEP_COPY)
            deep_copy_transfer_count++;
    }
    existing = xaot_bundle_find_capability_plan(bundle, XG_CAP_DEEP_COPY);
    if (existing)
        body_count = existing->body_count;
    return xaot_bundle_add_capability_plan(bundle, XG_CAP_DEEP_COPY, body_count,
                                           deep_copy_transfer_count);
}

XR_FUNC const XaotStaticDataPlan *xaot_bundle_find_static_data_plan(const XaotBundle *bundle,
                                                                    uint32_t static_data) {
    if (!bundle || static_data == 0)
        return NULL;
    for (uint32_t i = 0; i < bundle->nstatic_data_plans; i++) {
        if (bundle->static_data_plans[i].static_data == static_data)
            return &bundle->static_data_plans[i];
    }
    return NULL;
}

XR_FUNC const XaotLinkDependencyPlan *
xaot_bundle_find_link_dependency_plan(const XaotBundle *bundle, XgLinkId link_id) {
    if (!bundle || link_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < bundle->nlink_dependency_plans; i++) {
        if (bundle->link_dependency_plans[i].link_id == link_id)
            return &bundle->link_dependency_plans[i];
    }
    return NULL;
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
    xaot_enum_plan_finalize_scalarization(plan);
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
    xaot_enum_plan_finalize_scalarization(plan);
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
                                                         uint32_t flags,
                                                         const XgBodySummary *body) {
    XaotFuncAttrPlan *plan;

    /* CONST and PURE are mutually exclusive by definition. */
    if (!bundle || !func || !body || body->func_id == XG_NO_ID || flags == 0 ||
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
    plan->body_func_id = body->func_id;
    plan->body_effect_bits = body->effect_bits;
    plan->body_escape_bits = body->escape_bits;
    plan->evidence = XAOT_FN_ATTR_EV_BODY_SUMMARY | XAOT_FN_ATTR_EV_XI_EFFECT_SCAN;
    if ((body->effect_bits & XG_BODY_MAY_CALL) != 0)
        plan->evidence |= XAOT_FN_ATTR_EV_CALLEE_SUMMARY;
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
                                                    const XiValue *access,
                                                    const XgBodySummary *body, uint32_t evidence,
                                                    uint8_t unproven_reason) {
    XaotBoundsPlan *plan;

    /* Proven entries carry evidence and no reason; unproven entries carry a
     * reason and no evidence. Anything else is a caller bug. */
    if (!bundle || !func || !access || !body || body->func_id == XG_NO_ID ||
        (evidence == 0) == (unproven_reason == 0))
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
    plan->body_func_id = body->func_id;
    plan->body_effect_bits = body->effect_bits;
    plan->body_escape_bits = body->escape_bits;
    plan->body_evidence = XAOT_PLAN_BODY_EV_BODY_SUMMARY;
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

XR_FUNC XaotSpanAccessPlan *
xaot_bundle_add_span_access_plan(XaotBundle *bundle, const XiFunc *func, const XiValue *value,
                                 const XgBodySummary *body, uint8_t kind, uint32_t evidence,
                                 uint32_t eliminated_checks, uint8_t unproven_reason) {
    XaotSpanAccessPlan *plan;

    if (!bundle || !func || !value || !body || body->func_id == XG_NO_ID || kind == 0 ||
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
    plan->body_func_id = body->func_id;
    plan->body_effect_bits = body->effect_bits;
    plan->body_escape_bits = body->escape_bits;
    plan->body_evidence = XAOT_PLAN_BODY_EV_BODY_SUMMARY;
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
                                                  const XiValue *value, const XgBodySummary *body,
                                                  uint8_t kind, uint32_t evidence) {
    XaotAliasPlan *plan;

    if (!bundle || !func || !value || !body || body->func_id == XG_NO_ID || kind == 0 ||
        evidence == 0)
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
    plan->body_func_id = body->func_id;
    plan->body_effect_bits = body->effect_bits;
    plan->body_escape_bits = body->escape_bits;
    plan->body_evidence = XAOT_PLAN_BODY_EV_BODY_SUMMARY;
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

XR_FUNC XaotAllocationPlan *xaot_bundle_add_allocation_plan(XaotBundle *bundle, const XiFunc *func,
                                                            const XiValue *value,
                                                            const XgBodySummary *body,
                                                            uint8_t action, uint16_t original_op,
                                                            uint8_t escape, uint32_t evidence) {
    XaotAllocationPlan *plan;

    if (!bundle || !func || !value || !body || body->func_id == XG_NO_ID || action == 0 ||
        original_op == 0 || evidence == 0)
        return NULL;
    plan = (XaotAllocationPlan *) xaot_bundle_find_allocation_plan(bundle, value);
    if (plan)
        return plan;
    if (!reserve_plan_array((void **) &bundle->allocation_plans, &bundle->allocation_plan_cap,
                            bundle->nallocation_plans + 1, sizeof(XaotAllocationPlan), 16))
        return NULL;
    plan = &bundle->allocation_plans[bundle->nallocation_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->func = func;
    plan->value = value;
    plan->body_func_id = body->func_id;
    plan->body_effect_bits = body->effect_bits;
    plan->body_escape_bits = body->escape_bits;
    plan->body_evidence = XAOT_PLAN_BODY_EV_BODY_SUMMARY;
    plan->original_op = original_op;
    plan->escape = escape;
    plan->action = action;
    plan->evidence = evidence;
    if (!xaot_ptr_index_put(&bundle->allocation_index, value, bundle->nallocation_plans - 1)) {
        bundle->nallocation_plans--;
        memset(plan, 0, sizeof(*plan));
        bundle->error_msg = "failed to index AOT allocation plan";
        return NULL;
    }
    return plan;
}

XR_FUNC const XaotAllocationPlan *xaot_bundle_find_allocation_plan(const XaotBundle *bundle,
                                                                   const XiValue *value) {
    uint32_t idx;

    if (!bundle || !value)
        return NULL;
    if (xaot_ptr_index_get(&bundle->allocation_index, value, &idx) &&
        idx < bundle->nallocation_plans)
        return &bundle->allocation_plans[idx];
    return NULL;
}

XR_FUNC XaotClosurePlan *
xaot_bundle_add_closure_plan(XaotBundle *bundle, const XiFunc *func, const XiValue *value,
                             const XiFunc *target_func, uint16_t capture_count,
                             uint8_t representation, uint32_t evidence, uint8_t unproven_reason) {
    XaotClosurePlan *plan;

    if (!bundle || !func || !value || representation == 0 || evidence == 0)
        return NULL;
    plan = (XaotClosurePlan *) xaot_bundle_find_closure_plan(bundle, value);
    if (plan)
        return plan;
    if (!reserve_plan_array((void **) &bundle->closure_plans, &bundle->closure_plan_cap,
                            bundle->nclosure_plans + 1, sizeof(XaotClosurePlan), 16))
        return NULL;
    plan = &bundle->closure_plans[bundle->nclosure_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->func = func;
    plan->value = value;
    plan->target_func = target_func;
    plan->capture_count = capture_count;
    plan->representation = representation;
    plan->evidence = evidence;
    plan->unproven_reason = unproven_reason;
    if (!xaot_ptr_index_put(&bundle->closure_index, value, bundle->nclosure_plans - 1)) {
        bundle->nclosure_plans--;
        memset(plan, 0, sizeof(*plan));
        bundle->error_msg = "failed to index AOT closure plan";
        return NULL;
    }
    return plan;
}

XR_FUNC const XaotClosurePlan *xaot_bundle_find_closure_plan(const XaotBundle *bundle,
                                                             const XiValue *value) {
    uint32_t idx;

    if (!bundle || !value)
        return NULL;
    if (xaot_ptr_index_get(&bundle->closure_index, value, &idx) && idx < bundle->nclosure_plans)
        return &bundle->closure_plans[idx];
    return NULL;
}

XR_FUNC XaotTransferPlan *xaot_bundle_add_transfer_plan(
    XaotBundle *bundle, const XiFunc *func, const XiValue *site, uint16_t transfer_index,
    const XiValue *value, const XrType *value_type, const XaotTypeKey *value_type_key,
    uint8_t site_kind, uint8_t mode, uint8_t action, uint32_t evidence, uint8_t unproven_reason) {
    XaotTransferPlan *plan;

    if (!bundle || !func || !site || site_kind == 0 || action == 0 || evidence == 0)
        return NULL;
    plan = (XaotTransferPlan *) xaot_bundle_find_transfer_plan(bundle, site, transfer_index);
    if (plan)
        return plan;
    if (!reserve_plan_array((void **) &bundle->transfer_plans, &bundle->transfer_plan_cap,
                            bundle->ntransfer_plans + 1, sizeof(XaotTransferPlan), 16))
        return NULL;
    plan = &bundle->transfer_plans[bundle->ntransfer_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->func = func;
    plan->site = site;
    plan->value = value;
    plan->value_type = value_type;
    if (value_type_key)
        plan->value_type_key = *value_type_key;
    plan->transfer_index = transfer_index;
    plan->site_kind = site_kind;
    plan->mode = mode;
    plan->action = action;
    plan->evidence = evidence;
    plan->unproven_reason = unproven_reason;
    return plan;
}

XR_FUNC const XaotTransferPlan *xaot_bundle_find_transfer_plan(const XaotBundle *bundle,
                                                               const XiValue *site,
                                                               uint16_t transfer_index) {
    if (!bundle || !site)
        return NULL;
    for (uint32_t i = 0; i < bundle->ntransfer_plans; i++) {
        const XaotTransferPlan *plan = &bundle->transfer_plans[i];
        if (plan->site == site && plan->transfer_index == transfer_index)
            return plan;
    }
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

static const char *class_unproven_reason_name(uint8_t reason) {
    switch (reason) {
        case XAOT_CLASS_UNPROVEN_NONE:
            return "none";
        case XAOT_CLASS_UNPROVEN_NO_GLOBAL_EVIDENCE:
            return "no_global_evidence";
        case XAOT_CLASS_UNPROVEN_INCONSISTENT_GRAPH:
            return "inconsistent_graph";
        default:
            return "unknown";
    }
}

static const char *dispatch_kind_name(uint8_t kind) {
    switch ((XaotMethodDispatchKind) kind) {
        case XAOT_DISPATCH_DIRECT:
            return "direct";
        case XAOT_DISPATCH_VTABLE:
            return "vtable";
        case XAOT_DISPATCH_ITABLE:
            return "itable";
        case XAOT_DISPATCH_TYPE_SWITCH:
            return "type_switch";
        case XAOT_DISPATCH_RUNTIME_FALLBACK:
            return "runtime_fallback";
        default:
            return "unknown";
    }
}

static const char *dispatch_unproven_reason_name(uint8_t reason) {
    switch (reason) {
        case XAOT_DISPATCH_UNPROVEN_NONE:
            return "none";
        case XAOT_DISPATCH_UNPROVEN_NO_RECEIVER_TYPE:
            return "no_receiver_type";
        case XAOT_DISPATCH_UNPROVEN_NO_METHOD_ID:
            return "no_method_id";
        case XAOT_DISPATCH_UNPROVEN_POLYMORPHIC:
            return "polymorphic";
        case XAOT_DISPATCH_UNPROVEN_NO_INTERFACE_ID:
            return "no_interface_id";
        case XAOT_DISPATCH_UNPROVEN_NO_TARGET_METHOD:
            return "no_target_method";
        case XAOT_DISPATCH_UNPROVEN_LARGE_IMPLEMENTOR_SET:
            return "large_implementor_set";
        default:
            return "unknown";
    }
}

static const char *capability_action_name(uint32_t action) {
    switch ((XaotCapabilityProfileAction) action) {
        case XAOT_CAPABILITY_ACTION_ALLOW:
            return "allow";
        case XAOT_CAPABILITY_ACTION_LINK:
            return "link";
        case XAOT_CAPABILITY_ACTION_REJECT:
            return "reject";
        case XAOT_CAPABILITY_ACTION_DEBUG_ONLY:
            return "debug_only";
        default:
            return "unknown";
    }
}

static const char *capability_unproven_reason_name(uint8_t reason) {
    switch (reason) {
        case XAOT_CAPABILITY_UNPROVEN_NONE:
            return "none";
        case XAOT_CAPABILITY_UNPROVEN_NO_BODY:
            return "no_body";
        default:
            return "unknown";
    }
}

static const char *metadata_unproven_reason_name(uint8_t reason) {
    switch (reason) {
        case XAOT_METADATA_UNPROVEN_NONE:
            return "none";
        case XAOT_METADATA_UNPROVEN_NO_REACHABILITY:
            return "no_reachability";
        default:
            return "unknown";
    }
}

static const char *derive_action_name(uint8_t action) {
    switch ((XaotDeriveAction) action) {
        case XAOT_DERIVE_FIELD_TABLE_SIDECAR:
            return "field_table_sidecar";
        case XAOT_DERIVE_INLINE_GENERATED_BODY:
            return "inline_generated_body";
        case XAOT_DERIVE_METADATA_ONLY:
            return "metadata_only";
        case XAOT_DERIVE_DCE:
            return "dce";
        case XAOT_DERIVE_REJECT:
            return "reject";
        default:
            return "unknown";
    }
}

static const char *derive_unproven_reason_name(uint8_t reason) {
    switch (reason) {
        case XAOT_DERIVE_UNPROVEN_NONE:
            return "none";
        case XAOT_DERIVE_UNPROVEN_NO_REACHABILITY:
            return "no_reachability";
        case XAOT_DERIVE_UNPROVEN_INVALID_KIND:
            return "invalid_kind";
        default:
            return "unknown";
    }
}

static const char *derived_eq_hash_action_name(uint8_t action) {
    switch ((XaotDerivedEqHashAction) action) {
        case XAOT_DERIVED_EQ_HASH_BUILTIN_FIELDS_INLINE:
            return "builtin_fields_inline";
        case XAOT_DERIVED_EQ_HASH_RECURSIVE_DERIVE_INLINE:
            return "recursive_derive_inline";
        case XAOT_DERIVED_EQ_HASH_DIRECT_GENERATED_CALL:
            return "direct_generated_call";
        case XAOT_DERIVED_EQ_HASH_REJECT_UNHASHABLE:
            return "reject_unhashable";
        default:
            return "unknown";
    }
}

static const char *derived_eq_hash_unproven_reason_name(uint8_t reason) {
    switch (reason) {
        case XAOT_EQ_HASH_UNPROVEN_NONE:
            return "none";
        case XAOT_EQ_HASH_UNPROVEN_MISSING_EQ:
            return "missing_eq";
        case XAOT_EQ_HASH_UNPROVEN_MISSING_HASH:
            return "missing_hash";
        case XAOT_EQ_HASH_UNPROVEN_TYPE_MISMATCH:
            return "type_mismatch";
        case XAOT_EQ_HASH_UNPROVEN_FIELD_MISMATCH:
            return "field_mismatch";
        default:
            return "unknown";
    }
}

static const char *derived_clone_action_name(uint8_t action) {
    switch ((XaotDerivedCloneAction) action) {
        case XAOT_DERIVED_CLONE_BITWISE_COPY:
            return "bitwise_copy";
        case XAOT_DERIVED_CLONE_FIELDWISE_COPY:
            return "fieldwise_copy";
        case XAOT_DERIVED_CLONE_DEEP_COPY_PLAN:
            return "deep_copy_plan";
        case XAOT_DERIVED_CLONE_DIRECT_GENERATED_CALL:
            return "direct_generated_call";
        case XAOT_DERIVED_CLONE_REJECT:
            return "reject";
        default:
            return "unknown";
    }
}

static const char *derived_clone_unproven_reason_name(uint8_t reason) {
    switch (reason) {
        case XAOT_CLONE_UNPROVEN_NONE:
            return "none";
        case XAOT_CLONE_UNPROVEN_MISSING_CLONE:
            return "missing_clone";
        case XAOT_CLONE_UNPROVEN_UNSAFE_FIELD:
            return "unsafe_field";
        case XAOT_CLONE_UNPROVEN_MISSING_TRANSFER_PLAN:
            return "missing_transfer_plan";
        default:
            return "unknown";
    }
}

static const char *json_shape_action_name(uint8_t action) {
    switch ((XaotJsonShapeAction) action) {
        case XAOT_JSON_SHAPE_OPEN_DYNAMIC:
            return "open_dynamic";
        case XAOT_JSON_SHAPE_HIDDEN_CLASS:
            return "hidden_class";
        case XAOT_JSON_SHAPE_RECORD_BRIDGE:
            return "record_bridge";
        case XAOT_JSON_SHAPE_REJECT:
            return "reject";
        default:
            return "unknown";
    }
}

static const char *json_access_action_name(uint8_t action) {
    switch ((XaotJsonAccessAction) action) {
        case XAOT_JSON_ACCESS_DIRECT_INDEX:
            return "direct_index";
        case XAOT_JSON_ACCESS_SHAPE_GUARD_INDEX:
            return "shape_guard_index";
        case XAOT_JSON_ACCESS_COMPUTED_KEY_GUARD:
            return "computed_key_guard";
        case XAOT_JSON_ACCESS_DYNAMIC_LOOKUP:
            return "dynamic_lookup";
        case XAOT_JSON_ACCESS_REJECT:
            return "reject";
        default:
            return "unknown";
    }
}

static const char *json_codec_action_name(uint8_t action) {
    switch ((XaotJsonCodecAction) action) {
        case XAOT_JSON_CODEC_PARSE_DOM_BRIDGE:
            return "parse_dom_bridge";
        case XAOT_JSON_CODEC_PARSE_RUNTIME_DIRECT:
            return "parse_runtime_direct";
        case XAOT_JSON_CODEC_DECODE_VALIDATE_COPY:
            return "decode_validate_copy";
        case XAOT_JSON_CODEC_ENCODE_FIELD_TABLE:
            return "encode_field_table";
        case XAOT_JSON_CODEC_ENCODE_DERIVE_SIDECAR:
            return "encode_derive_sidecar";
        case XAOT_JSON_CODEC_STRINGIFY_DYNAMIC_WALK:
            return "stringify_dynamic_walk";
        case XAOT_JSON_CODEC_REJECT:
            return "reject";
        default:
            return "unknown";
    }
}

static const char *json_unproven_reason_name(uint8_t reason) {
    switch (reason) {
        case XAOT_JSON_UNPROVEN_NONE:
            return "none";
        case XAOT_JSON_UNPROVEN_COMPUTED_KEY:
            return "computed_key";
        case XAOT_JSON_UNPROVEN_RECEIVER_SHAPE_UNKNOWN:
            return "receiver_shape_unknown";
        case XAOT_JSON_UNPROVEN_STALE_SHAPE:
            return "stale_shape";
        case XAOT_JSON_UNPROVEN_INVALID_KIND:
            return "invalid_kind";
        case XAOT_JSON_UNPROVEN_OPEN_SHAPE:
            return "open_shape";
        case XAOT_JSON_UNPROVEN_MISSING_TARGET_TYPE:
            return "missing_target_type";
        case XAOT_JSON_UNPROVEN_UNSUPPORTED_CODEC:
            return "unsupported_codec";
        default:
            return "unknown";
    }
}

static const char *record_shape_action_name(uint8_t action) {
    switch ((XaotRecordShapeAction) action) {
        case XAOT_RECORD_SHAPE_SEALED_RECORD:
            return "sealed_record";
        case XAOT_RECORD_SHAPE_OPTIONS_BAG:
            return "options_bag";
        case XAOT_RECORD_SHAPE_SPREAD_RESULT:
            return "spread_result";
        case XAOT_RECORD_SHAPE_STATIC_RECORD:
            return "static_record";
        case XAOT_RECORD_SHAPE_REJECT:
            return "reject";
        default:
            return "unknown";
    }
}

static const char *record_access_action_name(uint8_t action) {
    switch ((XaotRecordAccessAction) action) {
        case XAOT_RECORD_ACCESS_DIRECT_FIELD:
            return "direct_field";
        case XAOT_RECORD_ACCESS_COPY_DESTRUCTURE:
            return "copy_destructure";
        case XAOT_RECORD_ACCESS_CHECKED_FIELD:
            return "checked_field";
        case XAOT_RECORD_ACCESS_REJECT:
            return "reject";
        default:
            return "unknown";
    }
}

static const char *record_unproven_reason_name(uint8_t reason) {
    switch (reason) {
        case XAOT_RECORD_UNPROVEN_NONE:
            return "none";
        case XAOT_RECORD_UNPROVEN_INVALID_KIND:
            return "invalid_kind";
        case XAOT_RECORD_UNPROVEN_RECEIVER_SHAPE_UNKNOWN:
            return "receiver_shape_unknown";
        case XAOT_RECORD_UNPROVEN_STALE_SHAPE:
            return "stale_shape";
        case XAOT_RECORD_UNPROVEN_DYNAMIC_FIELD:
            return "dynamic_field";
        default:
            return "unknown";
    }
}

static const char *options_action_name(uint8_t action) {
    switch ((XaotOptionsAction) action) {
        case XAOT_OPTIONS_DEFAULT_ELIDED:
            return "default_elided";
        case XAOT_OPTIONS_DEFAULT_FILL_TABLE:
            return "default_fill_table";
        case XAOT_OPTIONS_REQUIRED_CHECK:
            return "required_check";
        case XAOT_OPTIONS_CALLSITE_SPECIALIZED:
            return "callsite_specialized";
        case XAOT_OPTIONS_REJECT:
            return "reject";
        default:
            return "unknown";
    }
}

static const char *options_unproven_reason_name(uint8_t reason) {
    switch (reason) {
        case XAOT_OPTIONS_UNPROVEN_NONE:
            return "none";
        case XAOT_OPTIONS_UNPROVEN_INVALID_ACTION:
            return "invalid_action";
        case XAOT_OPTIONS_UNPROVEN_MISSING_CALLSITE:
            return "missing_callsite";
        case XAOT_OPTIONS_UNPROVEN_STALE_SHAPE:
            return "stale_shape";
        default:
            return "unknown";
    }
}

static const char *map_shape_action_name(uint8_t action) {
    switch ((XaotMapShapeAction) action) {
        case XAOT_MAP_SHAPE_RUNTIME_HASH:
            return "runtime_hash";
        case XAOT_MAP_SHAPE_PREALLOC_HASH:
            return "prealloc_hash";
        case XAOT_MAP_SHAPE_SMALL_INLINE:
            return "small_inline";
        case XAOT_MAP_SHAPE_DENSE_ENUM_TABLE:
            return "dense_enum_table";
        case XAOT_MAP_SHAPE_DENSE_INT_TABLE:
            return "dense_int_table";
        case XAOT_MAP_SHAPE_BOOL_DIRECT:
            return "bool_direct";
        case XAOT_MAP_SHAPE_READONLY_STATIC_TABLE:
            return "readonly_static_table";
        case XAOT_MAP_SHAPE_REJECT:
            return "reject";
        default:
            return "unknown";
    }
}

static const char *key_access_action_name(uint8_t action) {
    switch ((XaotKeyAccessAction) action) {
        case XAOT_KEY_ACCESS_DIRECT_DENSE_INDEX:
            return "direct_dense_index";
        case XAOT_KEY_ACCESS_BOOL_DIRECT_LOOKUP:
            return "bool_direct_lookup";
        case XAOT_KEY_ACCESS_PREHASHED_LOOKUP:
            return "prehashed_lookup";
        case XAOT_KEY_ACCESS_INLINE_SMALL_SCAN:
            return "inline_small_scan";
        case XAOT_KEY_ACCESS_SPECIALIZED_HASH_LOOKUP:
            return "specialized_hash_lookup";
        case XAOT_KEY_ACCESS_GENERIC_HASH_LOOKUP:
            return "generic_hash_lookup";
        case XAOT_KEY_ACCESS_REJECT:
            return "reject";
        default:
            return "unknown";
    }
}

static const char *hash_eq_action_name(uint8_t action) {
    switch ((XaotHashEqAction) action) {
        case XAOT_HASH_EQ_BUILTIN_INLINE:
            return "builtin_inline";
        case XAOT_HASH_EQ_DERIVE_INLINE:
            return "derive_inline";
        case XAOT_HASH_EQ_DIRECT_CALL:
            return "direct_call";
        case XAOT_HASH_EQ_DYNAMIC_REJECT:
            return "dynamic_reject";
        default:
            return "unknown";
    }
}

static const char *map_unproven_reason_name(uint8_t reason) {
    switch (reason) {
        case XAOT_MAP_UNPROVEN_NONE:
            return "none";
        case XAOT_MAP_UNPROVEN_INVALID_KIND:
            return "invalid_kind";
        case XAOT_MAP_UNPROVEN_COMPUTED_KEY:
            return "computed_key";
        case XAOT_MAP_UNPROVEN_MISSING_SHAPE:
            return "missing_shape";
        case XAOT_MAP_UNPROVEN_MISSING_HASH_EQ:
            return "missing_hash_eq";
        case XAOT_MAP_UNPROVEN_UNHASHABLE:
            return "unhashable";
        default:
            return "unknown";
    }
}

static const char *sequence_access_action_name(uint8_t action) {
    switch ((XaotSequenceAccessAction) action) {
        case XAOT_SEQUENCE_ACCESS_CHECKED_INDEX:
            return "checked_index";
        case XAOT_SEQUENCE_ACCESS_DIRECT_LENGTH:
            return "direct_length";
        case XAOT_SEQUENCE_ACCESS_CHECKED_SLICE:
            return "checked_slice";
        case XAOT_SEQUENCE_ACCESS_ITER_HELPER:
            return "iter_helper";
        case XAOT_SEQUENCE_ACCESS_REJECT:
            return "reject";
        default:
            return "unknown";
    }
}

static const char *sequence_unproven_reason_name(uint8_t reason) {
    switch (reason) {
        case XAOT_SEQUENCE_UNPROVEN_NONE:
            return "none";
        case XAOT_SEQUENCE_UNPROVEN_INVALID_KIND:
            return "invalid_kind";
        case XAOT_SEQUENCE_UNPROVEN_MISSING_RECEIVER_TYPE:
            return "missing_receiver_type";
        case XAOT_SEQUENCE_UNPROVEN_COMPUTED_INDEX:
            return "computed_index";
        case XAOT_SEQUENCE_UNPROVEN_NEGATIVE_INDEX:
            return "negative_index";
        case XAOT_SEQUENCE_UNPROVEN_DYNAMIC_LENGTH:
            return "dynamic_length";
        default:
            return "unknown";
    }
}

static void print_sequence_evidence_bits(FILE *out, uint32_t bits) {
    bool first = true;
#define PRINT_BIT(mask, name)                                                                      \
    do {                                                                                           \
        if ((bits & (mask)) != 0) {                                                                \
            fprintf(out, "%s%s", first ? "" : "+", (name));                                        \
            first = false;                                                                         \
        }                                                                                          \
    } while (0)
    PRINT_BIT(XAOT_SEQUENCE_EV_GLOBAL_ROW, "row");
    PRINT_BIT(XAOT_SEQUENCE_EV_RECEIVER_TYPE, "receiver_type");
    PRINT_BIT(XAOT_SEQUENCE_EV_ELEM_TYPE, "elem_type");
    PRINT_BIT(XAOT_SEQUENCE_EV_CONST_INDEX, "const_index");
    PRINT_BIT(XAOT_SEQUENCE_EV_LENGTH_EXPR, "length_expr");
    PRINT_BIT(XAOT_SEQUENCE_EV_MUTATING, "mutating");
    if (first)
        fprintf(out, "none");
#undef PRINT_BIT
}

static const char *capacity_action_name(uint8_t action) {
    switch ((XaotCapacityAction) action) {
        case XAOT_CAPACITY_CHECKED_GROW:
            return "checked_grow";
        case XAOT_CAPACITY_RESERVE_ONCE:
            return "reserve_once";
        case XAOT_CAPACITY_CLEAR_DIRECT:
            return "clear_direct";
        case XAOT_CAPACITY_BUILDER_FINISH:
            return "builder_finish";
        case XAOT_CAPACITY_RUNTIME_HELPER:
            return "runtime_helper";
        case XAOT_CAPACITY_REJECT:
            return "reject";
        default:
            return "unknown";
    }
}

static const char *capacity_unproven_reason_name(uint8_t reason) {
    switch (reason) {
        case XAOT_CAPACITY_UNPROVEN_NONE:
            return "none";
        case XAOT_CAPACITY_UNPROVEN_INVALID_KIND:
            return "invalid_kind";
        case XAOT_CAPACITY_UNPROVEN_MISSING_RECEIVER_TYPE:
            return "missing_receiver_type";
        case XAOT_CAPACITY_UNPROVEN_COUNT_UNKNOWN:
            return "count_unknown";
        default:
            return "unknown";
    }
}

static void print_capacity_evidence_bits(FILE *out, uint32_t bits) {
    bool first = true;
#define PRINT_BIT(mask, name)                                                                      \
    do {                                                                                           \
        if ((bits & (mask)) != 0) {                                                                \
            fprintf(out, "%s%s", first ? "" : "+", (name));                                        \
            first = false;                                                                         \
        }                                                                                          \
    } while (0)
    PRINT_BIT(XAOT_CAPACITY_EV_GLOBAL_ROW, "row");
    PRINT_BIT(XAOT_CAPACITY_EV_RECEIVER_TYPE, "receiver_type");
    PRINT_BIT(XAOT_CAPACITY_EV_ELEM_TYPE, "elem_type");
    PRINT_BIT(XAOT_CAPACITY_EV_EXACT_COUNT, "exact_count");
    PRINT_BIT(XAOT_CAPACITY_EV_LOOP_APPEND, "loop_append");
    PRINT_BIT(XAOT_CAPACITY_EV_MAY_GROW, "may_grow");
    if (first)
        fprintf(out, "none");
#undef PRINT_BIT
}

static const char *bulk_action_name(uint8_t action) {
    switch ((XaotBulkAction) action) {
        case XAOT_BULK_INLINE_MEMCPY:
            return "inline_memcpy";
        case XAOT_BULK_INLINE_MEMMOVE:
            return "inline_memmove";
        case XAOT_BULK_INLINE_MEMSET:
            return "inline_memset";
        case XAOT_BULK_INLINE_MEMCMP:
            return "inline_memcmp";
        case XAOT_BULK_TYPED_LOOP:
            return "typed_loop";
        case XAOT_BULK_RUNTIME_HELPER:
            return "runtime_helper";
        case XAOT_BULK_REJECT:
            return "reject";
        default:
            return "unknown";
    }
}

static const char *bulk_unproven_reason_name(uint8_t reason) {
    switch (reason) {
        case XAOT_BULK_UNPROVEN_NONE:
            return "none";
        case XAOT_BULK_UNPROVEN_INVALID_KIND:
            return "invalid_kind";
        case XAOT_BULK_UNPROVEN_NON_POD:
            return "non_pod";
        case XAOT_BULK_UNPROVEN_WRITE_BARRIER:
            return "write_barrier";
        case XAOT_BULK_UNPROVEN_LENGTH_UNKNOWN:
            return "length_unknown";
        default:
            return "unknown";
    }
}

static void print_bulk_evidence_bits(FILE *out, uint32_t bits) {
    bool first = true;
#define PRINT_BIT(mask, name)                                                                      \
    do {                                                                                           \
        if ((bits & (mask)) != 0) {                                                                \
            fprintf(out, "%s%s", first ? "" : "+", (name));                                        \
            first = false;                                                                         \
        }                                                                                          \
    } while (0)
    PRINT_BIT(XAOT_BULK_EV_GLOBAL_ROW, "row");
    PRINT_BIT(XAOT_BULK_EV_POD, "pod");
    PRINT_BIT(XAOT_BULK_EV_OVERLAP_POSSIBLE, "overlap_possible");
    PRINT_BIT(XAOT_BULK_EV_READONLY_SRC, "readonly_src");
    PRINT_BIT(XAOT_BULK_EV_WRITE_BARRIER, "write_barrier");
    PRINT_BIT(XAOT_BULK_EV_LENGTH_EXPR, "length_expr");
    if (first)
        fprintf(out, "none");
#undef PRINT_BIT
}

static const char *encoding_action_name(uint8_t action) {
    switch ((XaotEncodingAction) action) {
        case XAOT_ENCODING_VALIDATE_ELIDED:
            return "validate_elided";
        case XAOT_ENCODING_VALIDATE_ONCE:
            return "validate_once";
        case XAOT_ENCODING_RUNTIME_VALIDATE:
            return "runtime_validate";
        case XAOT_ENCODING_TRANSCODE:
            return "transcode";
        case XAOT_ENCODING_REJECT:
            return "reject";
        default:
            return "unknown";
    }
}

static const char *encoding_unproven_reason_name(uint8_t reason) {
    switch (reason) {
        case XAOT_ENCODING_UNPROVEN_NONE:
            return "none";
        case XAOT_ENCODING_UNPROVEN_INVALID_KIND:
            return "invalid_kind";
        case XAOT_ENCODING_UNPROVEN_RAW_BYTES_UNKNOWN:
            return "raw_bytes_unknown";
        default:
            return "unknown";
    }
}

static void print_encoding_evidence_bits(FILE *out, uint32_t bits) {
    bool first = true;
#define PRINT_BIT(mask, name)                                                                      \
    do {                                                                                           \
        if ((bits & (mask)) != 0) {                                                                \
            fprintf(out, "%s%s", first ? "" : "+", (name));                                        \
            first = false;                                                                         \
        }                                                                                          \
    } while (0)
    PRINT_BIT(XAOT_ENCODING_EV_GLOBAL_ROW, "row");
    PRINT_BIT(XAOT_ENCODING_EV_KNOWN_UTF8, "known_utf8");
    PRINT_BIT(XAOT_ENCODING_EV_VALIDATED_ONCE, "validated_once");
    PRINT_BIT(XAOT_ENCODING_EV_SCALAR_BOUNDARY, "scalar_boundary");
    PRINT_BIT(XAOT_ENCODING_EV_STATIC_LITERAL, "static_literal");
    PRINT_BIT(XAOT_ENCODING_EV_INPUT_TYPE, "input_type");
    PRINT_BIT(XAOT_ENCODING_EV_OUTPUT_TYPE, "output_type");
    if (first)
        fprintf(out, "none");
#undef PRINT_BIT
}

static const char *static_data_action_name(uint32_t action) {
    switch ((XaotStaticDataAction) action) {
        case XAOT_STATIC_DATA_ACTION_PROVE:
            return "prove";
        case XAOT_STATIC_DATA_ACTION_MATERIALIZE:
            return "materialize";
        case XAOT_STATIC_DATA_ACTION_RUNTIME_INIT:
            return "runtime_init";
        case XAOT_STATIC_DATA_ACTION_REJECT:
            return "reject";
        default:
            return "unknown";
    }
}

static const char *static_data_section_name(uint32_t section) {
    switch ((XaotStaticDataSection) section) {
        case XAOT_STATIC_DATA_SECTION_NONE:
            return "none";
        case XAOT_STATIC_DATA_SECTION_EVIDENCE:
            return "evidence";
        case XAOT_STATIC_DATA_SECTION_RODATA:
            return "rodata";
        case XAOT_STATIC_DATA_SECTION_RUNTIME_INIT:
            return "runtime_init";
        default:
            return "unknown";
    }
}

static const char *static_data_unproven_reason_name(uint8_t reason) {
    switch (reason) {
        case XAOT_STATIC_DATA_UNPROVEN_NONE:
            return "none";
        case XAOT_STATIC_DATA_UNPROVEN_NO_BODY:
            return "no_body";
        default:
            return "unknown";
    }
}

static void print_class_layout_flags(FILE *out, uint32_t flags) {
    bool first = true;
#define PRINT_BIT(mask, name)                                                                      \
    do {                                                                                           \
        if ((flags & (mask)) != 0) {                                                               \
            fprintf(out, "%s%s", first ? "" : "+", (name));                                        \
            first = false;                                                                         \
        }                                                                                          \
    } while (0)
    PRINT_BIT(XAOT_CLASS_LAYOUT_TYPED_PAYLOAD, "typed");
    PRINT_BIT(XAOT_CLASS_LAYOUT_PREFIX_PARENT, "prefix");
    PRINT_BIT(XAOT_CLASS_LAYOUT_TYPE_ID, "type_id");
    PRINT_BIT(XAOT_CLASS_LAYOUT_VTABLE, "vtable");
    PRINT_BIT(XAOT_CLASS_LAYOUT_HEADER, "header");
    if (first)
        fprintf(out, "none");
#undef PRINT_BIT
}

static void print_interface_reason_bits(FILE *out, uint32_t bits) {
    bool first = true;
#define PRINT_BIT(mask, name)                                                                      \
    do {                                                                                           \
        if ((bits & (mask)) != 0) {                                                                \
            fprintf(out, "%s%s", first ? "" : "+", (name));                                        \
            first = false;                                                                         \
        }                                                                                          \
    } while (0)
    PRINT_BIT(XAOT_INTERFACE_USE_REASON_IMPLEMENTS, "implements");
    PRINT_BIT(XAOT_INTERFACE_USE_REASON_VALUE, "value");
    PRINT_BIT(XAOT_INTERFACE_USE_REASON_ARRAY, "array");
    PRINT_BIT(XAOT_INTERFACE_USE_REASON_FIELD, "field");
    PRINT_BIT(XAOT_INTERFACE_USE_REASON_RETURN, "return");
    PRINT_BIT(XAOT_INTERFACE_USE_REASON_CAPTURE, "capture");
    PRINT_BIT(XAOT_INTERFACE_USE_REASON_PARAM, "param");
    if (first)
        fprintf(out, "none");
#undef PRINT_BIT
}

static void print_interface_flag_bits(FILE *out, uint32_t bits) {
    bool first = true;
#define PRINT_BIT(mask, name)                                                                      \
    do {                                                                                           \
        if ((bits & (mask)) != 0) {                                                                \
            fprintf(out, "%s%s", first ? "" : "+", (name));                                        \
            first = false;                                                                         \
        }                                                                                          \
    } while (0)
    PRINT_BIT(XAOT_INTERFACE_USE_NEEDS_IFACE_OBJECT, "iface_object");
    PRINT_BIT(XAOT_INTERFACE_USE_NEEDS_ITABLE, "itable");
    PRINT_BIT(XAOT_INTERFACE_USE_TYPE_SWITCHABLE, "type_switchable");
    if (first)
        fprintf(out, "none");
#undef PRINT_BIT
}

static const char *interface_abi_source_name(uint8_t source) {
    switch ((XaotInterfaceAbiSource) source) {
        case XAOT_INTERFACE_ABI_SOURCE_NONE:
            return "none";
        case XAOT_INTERFACE_ABI_SOURCE_BOXED_VALUE:
            return "boxed_value";
        case XAOT_INTERFACE_ABI_SOURCE_NATIVE_TYPE_ID:
            return "native_type_id";
        case XAOT_INTERFACE_ABI_SOURCE_DISPATCH_SLOT:
            return "dispatch_slot";
        default:
            return "unknown";
    }
}

static const char *interface_abi_unproven_reason_name(uint8_t reason) {
    switch (reason) {
        case XAOT_INTERFACE_ABI_UNPROVEN_NONE:
            return "none";
        case XAOT_INTERFACE_ABI_UNPROVEN_NO_CALLSITE:
            return "no_callsite";
        default:
            return "unknown";
    }
}

static void print_interface_abi_flag_bits(FILE *out, uint32_t bits) {
    bool first = true;
#define PRINT_BIT(mask, name)                                                                      \
    do {                                                                                           \
        if ((bits & (mask)) != 0) {                                                                \
            fprintf(out, "%s%s", first ? "" : "+", (name));                                        \
            first = false;                                                                         \
        }                                                                                          \
    } while (0)
    PRINT_BIT(XAOT_INTERFACE_ABI_NEEDS_IFACE_OBJECT, "iface_object");
    PRINT_BIT(XAOT_INTERFACE_ABI_NEEDS_ITABLE, "itable");
    PRINT_BIT(XAOT_INTERFACE_ABI_NEEDS_TYPE_SWITCH_TAG, "type_switch_tag");
    PRINT_BIT(XAOT_INTERFACE_ABI_BOXED_RECEIVER, "boxed_receiver");
    if (first)
        fprintf(out, "none");
#undef PRINT_BIT
}

static void print_interface_abi_evidence_bits(FILE *out, uint32_t bits) {
    bool first = true;
#define PRINT_BIT(mask, name)                                                                      \
    do {                                                                                           \
        if ((bits & (mask)) != 0) {                                                                \
            fprintf(out, "%s%s", first ? "" : "+", (name));                                        \
            first = false;                                                                         \
        }                                                                                          \
    } while (0)
    PRINT_BIT(XAOT_INTERFACE_ABI_EV_GLOBAL_CALLSITE, "callsite");
    PRINT_BIT(XAOT_INTERFACE_ABI_EV_INTERFACE_METHODS, "methods");
    PRINT_BIT(XAOT_INTERFACE_ABI_EV_IMPLEMENTOR_SET, "implementors");
    PRINT_BIT(XAOT_INTERFACE_ABI_EV_DISPATCH_PLAN, "dispatch");
    PRINT_BIT(XAOT_INTERFACE_ABI_EV_OBJECT_USE, "object_use");
    if (first)
        fprintf(out, "none");
#undef PRINT_BIT
}

static const char *specialization_action_name(uint8_t action) {
    switch ((XaotSpecializationAction) action) {
        case XAOT_SPECIALIZATION_DIRECT:
            return "direct";
        case XAOT_SPECIALIZATION_TYPE_SWITCH:
            return "type_switch";
        case XAOT_SPECIALIZATION_FALLBACK:
            return "fallback";
        default:
            return "unknown";
    }
}

static const char *specialization_unproven_reason_name(uint8_t reason) {
    switch (reason) {
        case XAOT_SPECIALIZATION_UNPROVEN_NONE:
            return "none";
        case XAOT_SPECIALIZATION_UNPROVEN_NO_INTERFACE:
            return "no_interface";
        case XAOT_SPECIALIZATION_UNPROVEN_NO_TARGET:
            return "no_target";
        case XAOT_SPECIALIZATION_UNPROVEN_LARGE_SET:
            return "large_set";
        case XAOT_SPECIALIZATION_UNPROVEN_DYNAMIC_BOUNDARY:
            return "dynamic_boundary";
        default:
            return "unknown";
    }
}

static void print_specialization_evidence_bits(FILE *out, uint32_t bits) {
    bool first = true;
#define PRINT_BIT(mask, name)                                                                      \
    do {                                                                                           \
        if ((bits & (mask)) != 0) {                                                                \
            fprintf(out, "%s%s", first ? "" : "+", (name));                                        \
            first = false;                                                                         \
        }                                                                                          \
    } while (0)
    PRINT_BIT(XAOT_SPECIALIZATION_EV_GLOBAL_CALLSITE, "callsite");
    PRINT_BIT(XAOT_SPECIALIZATION_EV_DISPATCH_PLAN, "dispatch");
    PRINT_BIT(XAOT_SPECIALIZATION_EV_IMPLEMENTOR_SET, "implementors");
    PRINT_BIT(XAOT_SPECIALIZATION_EV_TARGET_CASES, "targets");
    if (first)
        fprintf(out, "none");
#undef PRINT_BIT
}

static const char *generic_instantiation_action_name(uint8_t action) {
    switch ((XaotGenericInstantiationAction) action) {
        case XAOT_GENERIC_INSTANTIATION_RECORD_ROOT:
            return "record_root";
        case XAOT_GENERIC_INSTANTIATION_SPECIALIZED_BODY:
            return "specialized_body";
        case XAOT_GENERIC_INSTANTIATION_SPECIALIZED_ABI:
            return "specialized_abi";
        case XAOT_GENERIC_INSTANTIATION_SPECIALIZED_STORAGE:
            return "specialized_storage";
        default:
            return "unknown";
    }
}

static const char *generic_instantiation_unproven_reason_name(uint8_t reason) {
    switch (reason) {
        case XAOT_GENERIC_INST_UNPROVEN_NONE:
            return "none";
        case XAOT_GENERIC_INST_UNPROVEN_NO_SPECIALIZED_BODY:
            return "no_specialized_body";
        case XAOT_GENERIC_INST_UNPROVEN_NO_SPECIALIZED_STORAGE:
            return "no_specialized_storage";
        case XAOT_GENERIC_INST_UNPROVEN_NO_CONCRETE_TYPES:
            return "no_concrete_types";
        default:
            return "unknown";
    }
}

static void print_generic_instantiation_evidence_bits(FILE *out, uint32_t bits) {
    bool first = true;
#define PRINT_BIT(mask, name)                                                                      \
    do {                                                                                           \
        if ((bits & (mask)) != 0) {                                                                \
            fprintf(out, "%s%s", first ? "" : "+", (name));                                        \
            first = false;                                                                         \
        }                                                                                          \
    } while (0)
    PRINT_BIT(XAOT_GENERIC_INST_EV_GLOBAL_ROW, "row");
    PRINT_BIT(XAOT_GENERIC_INST_EV_CONCRETE_TYPES, "types");
    PRINT_BIT(XAOT_GENERIC_INST_EV_ORIGIN_ANCHOR, "origin");
    PRINT_BIT(XAOT_GENERIC_INST_EV_ROOT_CALLSITE, "root");
    PRINT_BIT(XAOT_GENERIC_INST_EV_INTERFACE_CONSTRAINT, "constraint");
    PRINT_BIT(XAOT_GENERIC_INST_EV_SPECIALIZED_BODY, "body");
    PRINT_BIT(XAOT_GENERIC_INST_EV_SPECIALIZED_ABI, "abi");
    PRINT_BIT(XAOT_GENERIC_INST_EV_SPECIALIZED_STORAGE, "storage");
    if (first)
        fprintf(out, "none");
#undef PRINT_BIT
}

static const char *generic_body_action_name(uint8_t action) {
    switch ((XaotGenericBodyAction) action) {
        case XAOT_GENERIC_BODY_CLONE:
            return "clone";
        case XAOT_GENERIC_BODY_SHARE_CANONICAL_BODY:
            return "share_canonical_body";
        case XAOT_GENERIC_BODY_DIRECT_CONSTRAINT_CALL:
            return "direct_constraint_call";
        case XAOT_GENERIC_BODY_REJECT:
            return "reject";
        default:
            return "unknown";
    }
}

static const char *generic_storage_action_name(uint8_t action) {
    switch ((XaotGenericStorageAction) action) {
        case XAOT_GENERIC_STORAGE_TYPED_INLINE:
            return "typed_inline";
        case XAOT_GENERIC_STORAGE_REF_LANE:
            return "ref_lane";
        case XAOT_GENERIC_STORAGE_BOXED:
            return "boxed";
        case XAOT_GENERIC_STORAGE_SPECIALIZED_CLASS:
            return "specialized_class";
        case XAOT_GENERIC_STORAGE_SPECIALIZED_STRUCT:
            return "specialized_struct";
        case XAOT_GENERIC_STORAGE_REJECT:
            return "reject";
        default:
            return "unknown";
    }
}

static const char *generic_code_size_action_name(uint8_t action) {
    switch ((XaotGenericCodeSizeAction) action) {
        case XAOT_GENERIC_CODESIZE_ALLOW_CLONE:
            return "allow_clone";
        case XAOT_GENERIC_CODESIZE_SHARE_CANONICAL_BODY:
            return "share_canonical_body";
        case XAOT_GENERIC_CODESIZE_FORCE_CLONE:
            return "force_clone";
        case XAOT_GENERIC_CODESIZE_REJECT:
            return "reject";
        default:
            return "unknown";
    }
}

static const char *generic_deepen_unproven_reason_name(uint8_t reason) {
    switch (reason) {
        case XAOT_GENERIC_DEEPEN_UNPROVEN_NONE:
            return "none";
        case XAOT_GENERIC_DEEPEN_UNPROVEN_MISSING_CONCRETE_TYPES:
            return "missing_concrete_types";
        case XAOT_GENERIC_DEEPEN_UNPROVEN_NO_SPECIALIZED_BODY:
            return "no_specialized_body";
        case XAOT_GENERIC_DEEPEN_UNPROVEN_UNSUPPORTED_STORAGE:
            return "unsupported_storage";
        case XAOT_GENERIC_DEEPEN_UNPROVEN_CODESIZE_THRESHOLD:
            return "code_size_threshold";
        case XAOT_GENERIC_DEEPEN_UNPROVEN_DYNAMIC_BOUNDARY:
            return "dynamic_boundary";
        default:
            return "unknown";
    }
}

static void print_generic_body_evidence_bits(FILE *out, uint32_t bits) {
    bool first = true;
#define PRINT_BIT(mask, name)                                                                      \
    do {                                                                                           \
        if ((bits & (mask)) != 0) {                                                                \
            fprintf(out, "%s%s", first ? "" : "+", (name));                                        \
            first = false;                                                                         \
        }                                                                                          \
    } while (0)
    PRINT_BIT(XAOT_GENERIC_BODY_EV_GLOBAL_ROW, "row");
    PRINT_BIT(XAOT_GENERIC_BODY_EV_GENERIC_INST, "inst");
    PRINT_BIT(XAOT_GENERIC_BODY_EV_TYPE_ARGS, "types");
    PRINT_BIT(XAOT_GENERIC_BODY_EV_ORIGIN_BODY, "origin_body");
    PRINT_BIT(XAOT_GENERIC_BODY_EV_SPECIALIZED_BODY, "specialized_body");
    PRINT_BIT(XAOT_GENERIC_BODY_EV_ROOT_CALLSITE, "root");
    if (first)
        fprintf(out, "none");
#undef PRINT_BIT
}

static void print_generic_storage_evidence_bits(FILE *out, uint32_t bits) {
    bool first = true;
#define PRINT_BIT(mask, name)                                                                      \
    do {                                                                                           \
        if ((bits & (mask)) != 0) {                                                                \
            fprintf(out, "%s%s", first ? "" : "+", (name));                                        \
            first = false;                                                                         \
        }                                                                                          \
    } while (0)
    PRINT_BIT(XAOT_GENERIC_STORAGE_EV_GLOBAL_ROW, "row");
    PRINT_BIT(XAOT_GENERIC_STORAGE_EV_GENERIC_INST, "inst");
    PRINT_BIT(XAOT_GENERIC_STORAGE_EV_SPECIALIZED_TYPE, "specialized_type");
    PRINT_BIT(XAOT_GENERIC_STORAGE_EV_CONTAINER_PLAN, "container_plan");
    if (first)
        fprintf(out, "none");
#undef PRINT_BIT
}

static void print_generic_code_size_evidence_bits(FILE *out, uint32_t bits) {
    bool first = true;
#define PRINT_BIT(mask, name)                                                                      \
    do {                                                                                           \
        if ((bits & (mask)) != 0) {                                                                \
            fprintf(out, "%s%s", first ? "" : "+", (name));                                        \
            first = false;                                                                         \
        }                                                                                          \
    } while (0)
    PRINT_BIT(XAOT_GENERIC_CODESIZE_EV_GLOBAL_ROW, "row");
    PRINT_BIT(XAOT_GENERIC_CODESIZE_EV_GENERIC_INST, "inst");
    PRINT_BIT(XAOT_GENERIC_CODESIZE_EV_BODY_USE, "body_use");
    PRINT_BIT(XAOT_GENERIC_CODESIZE_EV_THRESHOLD, "threshold");
    if (first)
        fprintf(out, "none");
#undef PRINT_BIT
}

static void print_bounds_evidence_bits(FILE *out, uint32_t bits) {
    bool first = true;
#define PRINT_BIT(mask, name)                                                                      \
    do {                                                                                           \
        if ((bits & (mask)) != 0) {                                                                \
            fprintf(out, "%s%s", first ? "" : "+", (name));                                        \
            first = false;                                                                         \
        }                                                                                          \
    } while (0)
    PRINT_BIT(XAOT_BOUNDS_EV_DOM_GUARD, "dom_guard");
    PRINT_BIT(XAOT_BOUNDS_EV_COUNTED_LOOP, "counted_loop");
    PRINT_BIT(XAOT_BOUNDS_EV_NONNEG_INDEX, "nonneg");
    PRINT_BIT(XAOT_BOUNDS_EV_NO_CLOBBER, "no_clobber");
    if (first)
        fprintf(out, "none");
#undef PRINT_BIT
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

static const char *closure_representation_name(uint8_t representation) {
    switch ((XaotClosureRepresentation) representation) {
        case XAOT_CLOSURE_RUNTIME:
            return "runtime";
        case XAOT_CLOSURE_STACK:
            return "stack";
        case XAOT_CLOSURE_DIRECT_SYMBOL:
            return "direct_symbol";
        default:
            return "unknown";
    }
}

static const char *allocation_action_name(uint8_t action) {
    switch ((XaotAllocationAction) action) {
        case XAOT_ALLOC_ACTION_NONE:
            return "none";
        case XAOT_ALLOC_ACTION_STACK:
            return "stack";
        case XAOT_ALLOC_ACTION_SROA:
            return "sroa";
        default:
            return "unknown";
    }
}

static const char *closure_unproven_reason_name(uint8_t reason) {
    switch (reason) {
        case XAOT_CLOSURE_UNPROVEN_NONE:
            return "none";
        case XAOT_CLOSURE_UNPROVEN_NO_TARGET:
            return "no_target";
        case XAOT_CLOSURE_UNPROVEN_CAPTURE_ARITY:
            return "capture_arity";
        default:
            return "unknown";
    }
}

static void print_closure_evidence_bits(FILE *out, uint32_t bits) {
    bool first = true;
#define PRINT_BIT(mask, name)                                                                      \
    do {                                                                                           \
        if ((bits & (mask)) != 0) {                                                                \
            fprintf(out, "%s%s", first ? "" : "+", (name));                                        \
            first = false;                                                                         \
        }                                                                                          \
    } while (0)
    PRINT_BIT(XAOT_CLOSURE_EV_XI_VALUE, "xi_value");
    PRINT_BIT(XAOT_CLOSURE_EV_TARGET_FUNC, "target");
    PRINT_BIT(XAOT_CLOSURE_EV_CAPTURE_ARITY, "captures");
    PRINT_BIT(XAOT_CLOSURE_EV_NOESCAPE_STACK, "noescape_stack");
    PRINT_BIT(XAOT_CLOSURE_EV_DIRECT_SYMBOL, "direct_symbol");
    if (first)
        fprintf(out, "none");
#undef PRINT_BIT
}

static const char *transfer_site_kind_name(uint8_t kind) {
    switch ((XaotTransferSiteKind) kind) {
        case XAOT_TRANSFER_GO_ARG:
            return "go_arg";
        case XAOT_TRANSFER_THREAD_ARG:
            return "thread_arg";
        case XAOT_TRANSFER_CHAN_SEND:
            return "chan_send";
        case XAOT_TRANSFER_CHAN_TRY_SEND:
            return "chan_try_send";
        case XAOT_TRANSFER_CHAN_SEND_TIMEOUT:
            return "chan_send_timeout";
        default:
            return "unknown";
    }
}

static const char *transfer_mode_name(uint8_t mode) {
    switch ((XrTransferMode) mode) {
        case XR_TRANSFER_SHARE:
            return "share";
        case XR_TRANSFER_COPY:
            return "copy";
        case XR_TRANSFER_MOVE:
            return "move";
        default:
            return "unknown";
    }
}

static const char *transfer_action_name(uint8_t action) {
    switch ((XaotTransferAction) action) {
        case XAOT_TRANSFER_ACTION_SHARE:
            return "share";
        case XAOT_TRANSFER_ACTION_COPY:
            return "copy";
        case XAOT_TRANSFER_ACTION_MOVE:
            return "move";
        case XAOT_TRANSFER_ACTION_DEEP_COPY:
            return "deep_copy";
        case XAOT_TRANSFER_ACTION_REJECT:
            return "reject";
        default:
            return "unknown";
    }
}

static const char *transfer_unproven_reason_name(uint8_t reason) {
    switch (reason) {
        case XAOT_TRANSFER_UNPROVEN_NONE:
            return "none";
        case XAOT_TRANSFER_UNPROVEN_NO_VALUE:
            return "no_value";
        case XAOT_TRANSFER_UNPROVEN_BAD_MODE:
            return "bad_mode";
        default:
            return "unknown";
    }
}

static const char *enum_scalar_action_name(uint8_t action) {
    switch ((XaotEnumScalarAction) action) {
        case XAOT_ENUM_SCALAR_RUNTIME_AGGREGATE:
            return "runtime_aggregate";
        case XAOT_ENUM_SCALAR_COMPACT_AGGREGATE:
            return "compact_aggregate";
        default:
            return "unknown";
    }
}

static void print_transfer_evidence_bits(FILE *out, uint32_t bits) {
    bool first = true;
#define PRINT_BIT(mask, name)                                                                      \
    do {                                                                                           \
        if ((bits & (mask)) != 0) {                                                                \
            fprintf(out, "%s%s", first ? "" : "+", (name));                                        \
            first = false;                                                                         \
        }                                                                                          \
    } while (0)
    PRINT_BIT(XAOT_TRANSFER_EV_SITE, "site");
    PRINT_BIT(XAOT_TRANSFER_EV_VALUE, "value");
    PRINT_BIT(XAOT_TRANSFER_EV_MODE, "mode");
    PRINT_BIT(XAOT_TRANSFER_EV_TYPE, "type");
    PRINT_BIT(XAOT_TRANSFER_EV_BOUNDARY_CLONE, "boundary_clone");
    if (first)
        fprintf(out, "none");
#undef PRINT_BIT
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
    if (bundle->global_evidence_plan.evidence) {
        const XgGlobalEvidence *ev = bundle->global_evidence_plan.evidence;
        fprintf(out,
                "global-evidence profile=%s hash=%016" PRIx64
                " decls=%u classes=%u methods=%u interface_impls=%u interface_extends=%u "
                "interface_methods=%u bodies=%u callsites=%u link_deps=%u generic_insts=%u\n",
                xg_build_profile_name(bundle->global_evidence_plan.profile),
                bundle->global_evidence_plan.evidence_hash, ev->ndecls, ev->nclasses, ev->nmethods,
                ev->ninterface_impls, ev->ninterface_extends, ev->ninterface_methods, ev->nbodies,
                ev->ncallsites, ev->nlink_deps, ev->ngeneric_insts);
    } else {
        fprintf(out, "global-evidence profile=none hash=0000000000000000 decls=0 classes=0 "
                     "methods=0 interface_impls=0 interface_extends=0 interface_methods=0 bodies=0 "
                     "callsites=0 link_deps=0 generic_insts=0\n");
    }
    for (mi = 0; mi < bundle->nmodules; mi++) {
        const XiModule *mod = bundle->modules ? bundle->modules[mi] : NULL;
        const XiFunc *init = mod ? mod->init : NULL;
        fprintf(out, "module %u name=%s path=%s entry=%u funcs=%u\n", mi,
                safe_str(mod ? mod->name : NULL), safe_str(mod ? mod->path : NULL),
                mi == bundle->entry_module ? 1u : 0u,
                init ? (unsigned) (1u + init->nchildren) : 0u);
    }

    for (uint32_t ci = 0; ci < bundle->nclass_hierarchy_plans; ci++) {
        const XaotClassHierarchyPlan *cp = &bundle->class_hierarchy_plans[ci];
        fprintf(out,
                "class-hierarchy %u id=%u parent=%u explicit_final=%u has_subclass=%u "
                "inferred_final=%u evidence=0x%x reason=%s\n",
                ci, cp->class_id, cp->parent_class_id, (cp->flags & XG_CLASS_EXPLICIT_FINAL) != 0,
                (cp->flags & XG_CLASS_HAS_SUBCLASS) != 0,
                (cp->flags & XG_CLASS_INFERRED_FINAL) != 0, cp->evidence,
                class_unproven_reason_name(cp->unproven_reason));
    }

    for (uint32_t ci = 0; ci < bundle->nclass_layout_plans; ci++) {
        const XaotClassLayoutPlan *cp = &bundle->class_layout_plans[ci];
        fprintf(out, "class-layout %u id=%u ctype=%s size=%u align=%u fields=%u+%u flags=", ci,
                cp->class_id, safe_str(cp->c_type_name), cp->instance_size, cp->instance_align,
                cp->field_start, cp->field_count);
        print_class_layout_flags(out, cp->flags);
        fprintf(out, "\n");
    }

    for (uint32_t di = 0; di < bundle->nmethod_dispatch_plans; di++) {
        const XaotMethodDispatchPlan *dp = &bundle->method_dispatch_plans[di];
        if (dp->dispatch_slot == UINT32_MAX) {
            fprintf(out,
                    "method-dispatch %u callsite=%u span=%u kind=%s owner=%u ordinal=%u method=%u "
                    "root=%u method_name=%u method_sig=%u args=%u+%u recv_class=%u recv_iface=%u "
                    "slot=- targets=%u+%u evidence=0x%x reason=%s\n",
                    di, dp->callsite_id, dp->source_span_id, dispatch_kind_name(dp->kind),
                    dp->owner_func_id, dp->body_ordinal, dp->method_id, dp->method_root_id,
                    dp->method_name_id, dp->method_signature_key, dp->arg_type_key_start,
                    (unsigned) dp->arg_count, dp->receiver_static_class_id,
                    dp->receiver_static_interface_id, dp->target_start, (unsigned) dp->target_count,
                    dp->evidence, dispatch_unproven_reason_name(dp->unproven_reason));
        } else {
            fprintf(out,
                    "method-dispatch %u callsite=%u span=%u kind=%s owner=%u ordinal=%u method=%u "
                    "root=%u method_name=%u method_sig=%u args=%u+%u recv_class=%u recv_iface=%u "
                    "slot=%u targets=%u+%u evidence=0x%x reason=%s\n",
                    di, dp->callsite_id, dp->source_span_id, dispatch_kind_name(dp->kind),
                    dp->owner_func_id, dp->body_ordinal, dp->method_id, dp->method_root_id,
                    dp->method_name_id, dp->method_signature_key, dp->arg_type_key_start,
                    (unsigned) dp->arg_count, dp->receiver_static_class_id,
                    dp->receiver_static_interface_id, dp->dispatch_slot, dp->target_start,
                    (unsigned) dp->target_count, dp->evidence,
                    dispatch_unproven_reason_name(dp->unproven_reason));
        }
    }

    for (uint32_t ti = 0; ti < bundle->ndispatch_target_cases; ti++) {
        const XaotDispatchTargetCase *tc = &bundle->dispatch_target_cases[ti];
        fprintf(out,
                "dispatch-target %u callsite=%u recv_class=%u method=%u owner_class=%u body=%u "
                "name=%u sig=%u root=%u depth=%u evidence=0x%x\n",
                ti, tc->callsite_id, tc->receiver_class_id, tc->method_id,
                tc->method_owner_class_id, tc->method_body_func_id, tc->method_name_id,
                tc->method_signature_key, tc->method_root_id, tc->method_override_depth,
                tc->evidence);
    }

    for (uint32_t ii = 0; ii < bundle->ninterface_use_plans; ii++) {
        const XaotInterfaceUsePlan *ip = &bundle->interface_use_plans[ii];
        fprintf(out, "interface-use %u interface=%u implementor=%u use_site=%u reason=", ii,
                ip->interface_id, ip->implementor_class_id, ip->use_site_id);
        print_interface_reason_bits(out, ip->reason);
        fprintf(out, " flags=");
        print_interface_flag_bits(out, ip->flags);
        fprintf(out, "\n");
    }

    for (uint32_t ai = 0; ai < bundle->ninterface_abi_plans; ai++) {
        const XaotInterfaceAbiPlan *ap = &bundle->interface_abi_plans[ai];
        fprintf(out,
                "interface-abi %u interface=%u callsites=%u implementors=%u slots=%u flags=", ai,
                ap->interface_id, ap->callsite_count, ap->implementor_count, ap->method_slot_count);
        print_interface_abi_flag_bits(out, ap->flags);
        fprintf(out, " data=%s type=%s itable=%s tag=%s evidence=",
                interface_abi_source_name(ap->data_source),
                interface_abi_source_name(ap->type_source),
                interface_abi_source_name(ap->itable_source),
                interface_abi_source_name(ap->tag_source));
        print_interface_abi_evidence_bits(out, ap->evidence);
        fprintf(out, " reason=%s\n", interface_abi_unproven_reason_name(ap->unproven_reason));
    }

    for (uint32_t gi = 0; gi < bundle->ngeneric_specialization_plans; gi++) {
        const XaotGenericSpecializationPlan *gp = &bundle->generic_specialization_plans[gi];
        fprintf(out,
                "generic-specialization %u callsite=%u owner=%u interface=%u method_name=%u "
                "method_sig=%u action=%s dispatch=%s implementors=%u single=%u targets=%u "
                "evidence=",
                gi, gp->callsite_id, gp->owner_func_id, gp->interface_id, gp->method_name_id,
                gp->method_signature_key, specialization_action_name(gp->action),
                dispatch_kind_name(gp->dispatch_kind), gp->implementor_count,
                gp->single_implementor_class_id, (unsigned) gp->target_count);
        print_specialization_evidence_bits(out, gp->evidence);
        fprintf(out, " reason=%s\n", specialization_unproven_reason_name(gp->unproven_reason));
    }

    for (uint32_t gi = 0; gi < bundle->ngeneric_instantiation_plans; gi++) {
        const XaotGenericInstantiationPlan *gp = &bundle->generic_instantiation_plans[gi];
        fprintf(out,
                "generic-instantiation %u id=%u module=%u kind=%s origin_decl=%u "
                "origin_func=%u origin_method=%u origin_class=%u specialized_func=%u "
                "specialized_class=%u root_callsite=%u constraint_iface=%u name=%u type=%u "
                "type_args=%u+%u action=%s evidence=",
                gi, gp->generic_inst_id, gp->module_id, xg_generic_inst_kind_name(gp->inst_kind),
                gp->origin_decl_id, gp->origin_func_id, gp->origin_method_id, gp->origin_class_id,
                gp->specialized_func_id, gp->specialized_class_id, gp->root_callsite_id,
                gp->constraint_interface_id, gp->name_id, gp->type_key, gp->type_arg_key_start,
                (unsigned) gp->type_arg_count, generic_instantiation_action_name(gp->action));
        print_generic_instantiation_evidence_bits(out, gp->evidence);
        fprintf(out, " reason=%s\n",
                generic_instantiation_unproven_reason_name(gp->unproven_reason));
    }

    for (uint32_t gi = 0; gi < bundle->ngeneric_body_plans; gi++) {
        const XaotGenericBodyPlan *gp = &bundle->generic_body_plans[gi];
        fprintf(out,
                "generic-body-plan %u id=%u inst=%u module=%u owner=%u origin_body=%u "
                "specialized_body=%u root_callsite=%u type=%u type_args=%u+%u size=%u "
                "action=%s evidence=",
                gi, gp->use_id, gp->generic_inst_id, gp->module_id, gp->owner_func_id,
                gp->origin_body_func_id, gp->specialized_body_func_id, gp->root_callsite_id,
                gp->type_key, gp->type_arg_key_start, (unsigned) gp->type_arg_count,
                gp->estimated_body_size, generic_body_action_name(gp->action));
        print_generic_body_evidence_bits(out, gp->evidence);
        fprintf(out, " reason=%s\n", generic_deepen_unproven_reason_name(gp->unproven_reason));
    }

    for (uint32_t gi = 0; gi < bundle->ngeneric_storage_plans; gi++) {
        const XaotGenericStoragePlan *gp = &bundle->generic_storage_plans[gi];
        fprintf(out,
                "generic-storage-plan %u id=%u inst=%u module=%u kind=%s action=%s "
                "origin_type=%u specialized_type=%u elem_type=%u key_type=%u value_type=%u "
                "container_plan=%u evidence=",
                gi, gp->storage_id, gp->generic_inst_id, gp->module_id,
                xg_generic_storage_kind_name(gp->storage_kind),
                generic_storage_action_name(gp->action), gp->origin_type_key,
                gp->specialized_type_key, gp->elem_type_key, gp->key_type_key, gp->value_type_key,
                gp->container_plan_id);
        print_generic_storage_evidence_bits(out, gp->evidence);
        fprintf(out, " reason=%s\n", generic_deepen_unproven_reason_name(gp->unproven_reason));
    }

    for (uint32_t gi = 0; gi < bundle->ngeneric_code_size_plans; gi++) {
        const XaotGenericCodeSizePlan *gp = &bundle->generic_code_size_plans[gi];
        fprintf(out,
                "generic-code-size-plan %u id=%u inst=%u module=%u body_use=%u origin=%u "
                "specialized=%u count=%u threshold=%u action=%s evidence=",
                gi, gp->code_size_id, gp->generic_inst_id, gp->module_id, gp->body_use_id,
                gp->origin_body_size_estimate, gp->specialized_body_size_estimate,
                gp->instantiation_count, gp->threshold, generic_code_size_action_name(gp->action));
        print_generic_code_size_evidence_bits(out, gp->evidence);
        fprintf(out, " reason=%s\n", generic_deepen_unproven_reason_name(gp->unproven_reason));
    }

    for (uint32_t di = 0; di < bundle->nderive_plans; di++) {
        const XaotDerivePlan *dp = &bundle->derive_plans[di];
        fprintf(out,
                "derive-plan %u id=%u decl=%u type=%u kind=%s action=%s fields=%u+%u "
                "methods=%u+%u sidecar=%u body=%u evidence=0x%x reason=%s\n",
                di, dp->derive_id, dp->owner_decl_id, dp->type_key,
                xg_derive_kind_name(dp->derive_kind), derive_action_name(dp->action),
                dp->field_start, (unsigned) dp->field_count, dp->method_start,
                (unsigned) dp->method_count, dp->sidecar_index, dp->generated_body_func_id,
                dp->evidence, derive_unproven_reason_name(dp->unproven_reason));
    }

    for (uint32_t di = 0; di < bundle->nderived_eq_hash_plans; di++) {
        const XaotDerivedEqHashPlan *dp = &bundle->derived_eq_hash_plans[di];
        fprintf(out,
                "derived-eq-hash-plan %u decl=%u type=%u eq=%u hash=%u action=%s "
                "fields=%u+%u eq_body=%u hash_body=%u evidence=0x%x reason=%s\n",
                di, dp->owner_decl_id, dp->type_key, dp->eq_derive_id, dp->hash_derive_id,
                derived_eq_hash_action_name(dp->action), dp->field_start,
                (unsigned) dp->field_count, dp->eq_body_func_id, dp->hash_body_func_id,
                dp->evidence, derived_eq_hash_unproven_reason_name(dp->unproven_reason));
    }

    for (uint32_t di = 0; di < bundle->nderived_clone_plans; di++) {
        const XaotDerivedClonePlan *dp = &bundle->derived_clone_plans[di];
        fprintf(out,
                "derived-clone-plan %u decl=%u type=%u clone=%u action=%s fields=%u+%u "
                "body=%u transfer=%u evidence=0x%x reason=%s\n",
                di, dp->owner_decl_id, dp->type_key, dp->clone_derive_id,
                derived_clone_action_name(dp->action), dp->field_start, (unsigned) dp->field_count,
                dp->clone_body_func_id, dp->transfer_plan_id, dp->evidence,
                derived_clone_unproven_reason_name(dp->unproven_reason));
    }

    for (uint32_t ji = 0; ji < bundle->njson_shape_plans; ji++) {
        const XaotJsonShapePlan *jp = &bundle->json_shape_plans[ji];
        fprintf(out,
                "json-shape-plan %u id=%u module=%u func=%u type=%u kind=%s action=%s "
                "fields=%u+%u hash=%016" PRIx64 " evidence=0x%x reason=%s\n",
                ji, jp->json_shape_id, jp->module_id, jp->owner_func_id, jp->type_key,
                xg_json_shape_kind_name(jp->shape_kind), json_shape_action_name(jp->action),
                jp->field_name_start, (unsigned) jp->field_count, jp->shape_hash, jp->evidence,
                json_unproven_reason_name(jp->unproven_reason));
    }

    for (uint32_t ji = 0; ji < bundle->njson_access_plans; ji++) {
        const XaotJsonAccessPlan *jp = &bundle->json_access_plans[ji];
        fprintf(out,
                "json-access-plan %u id=%u module=%u func=%u shape=%u kind=%s action=%s "
                "key=%u result_type=%u field=%u evidence=0x%x reason=%s\n",
                ji, jp->json_access_id, jp->module_id, jp->owner_func_id, jp->receiver_shape_id,
                xg_json_access_kind_name(jp->access_kind), json_access_action_name(jp->action),
                jp->key_name_id, jp->result_type_key, (unsigned) jp->field_ordinal, jp->evidence,
                json_unproven_reason_name(jp->unproven_reason));
    }

    for (uint32_t ji = 0; ji < bundle->njson_codec_plans; ji++) {
        const XaotJsonCodecPlan *jp = &bundle->json_codec_plans[ji];
        fprintf(out,
                "json-codec-plan %u id=%u module=%u func=%u span=%u kind=%s action=%s "
                "input_type=%u target_type=%u input_shape=%u output_shape=%u fields=%u "
                "evidence=0x%x reason=%s\n",
                ji, jp->codec_id, jp->module_id, jp->owner_func_id, jp->source_span_id,
                xg_json_codec_kind_name(jp->codec_kind), json_codec_action_name(jp->action),
                jp->input_type_key, jp->target_type_key, jp->input_shape_id, jp->output_shape_id,
                (unsigned) jp->field_count, jp->evidence,
                json_unproven_reason_name(jp->unproven_reason));
    }

    for (uint32_t ri = 0; ri < bundle->nrecord_shape_plans; ri++) {
        const XaotRecordShapePlan *rp = &bundle->record_shape_plans[ri];
        fprintf(out,
                "record-shape-plan %u id=%u module=%u func=%u type=%u kind=%s action=%s "
                "fields=%u+%u hash=%016" PRIx64 " evidence=0x%x reason=%s\n",
                ri, rp->record_shape_id, rp->module_id, rp->owner_func_id, rp->type_key,
                xg_record_shape_kind_name(rp->shape_kind), record_shape_action_name(rp->action),
                rp->field_name_start, (unsigned) rp->field_count, rp->shape_hash, rp->evidence,
                record_unproven_reason_name(rp->unproven_reason));
    }

    for (uint32_t ri = 0; ri < bundle->nrecord_access_plans; ri++) {
        const XaotRecordAccessPlan *rp = &bundle->record_access_plans[ri];
        fprintf(out,
                "record-access-plan %u id=%u module=%u func=%u shape=%u kind=%s action=%s "
                "field_name=%u result_type=%u field=%u evidence=0x%x reason=%s\n",
                ri, rp->record_access_id, rp->module_id, rp->owner_func_id, rp->receiver_shape_id,
                xg_record_access_kind_name(rp->access_kind), record_access_action_name(rp->action),
                rp->field_name_id, rp->result_type_key, (unsigned) rp->field_ordinal, rp->evidence,
                record_unproven_reason_name(rp->unproven_reason));
    }

    for (uint32_t oi = 0; oi < bundle->noptions_plans; oi++) {
        const XaotOptionsPlan *op = &bundle->options_plans[oi];
        fprintf(out,
                "options-plan %u id=%u module=%u func=%u callsite=%u param_shape=%u "
                "supplied_shape=%u action=%s supplied_mask=%u default_mask=%u required_mask=%u "
                "supplied=%u defaults=%u required=%u evidence=0x%x reason=%s\n",
                oi, op->options_id, op->module_id, op->owner_func_id, op->callsite_id,
                op->param_shape_id, op->supplied_shape_id, options_action_name(op->action),
                op->supplied_field_mask_id, op->default_field_mask_id, op->required_field_mask_id,
                (unsigned) op->supplied_count, (unsigned) op->default_count,
                (unsigned) op->required_count, op->evidence,
                options_unproven_reason_name(op->unproven_reason));
    }

    for (uint32_t mi = 0; mi < bundle->nmap_shape_plans; mi++) {
        const XaotMapShapePlan *mp = &bundle->map_shape_plans[mi];
        fprintf(out,
                "map-shape-plan %u id=%u module=%u func=%u container=%s source=%s action=%s "
                "key_type=%u value_type=%u entries=%u+%u literal_count=%u hash=%016" PRIx64
                " evidence=0x%x reason=%s\n",
                mi, mp->shape_id, mp->module_id, mp->owner_func_id,
                xg_map_container_kind_name(mp->container_kind),
                xg_map_shape_source_name(mp->source), map_shape_action_name(mp->action),
                mp->key_type_key, mp->value_type_key, mp->entry_start, (unsigned) mp->entry_count,
                mp->literal_count, mp->shape_hash, mp->evidence,
                map_unproven_reason_name(mp->unproven_reason));
    }

    for (uint32_t hi = 0; hi < bundle->nhash_eq_plans; hi++) {
        const XaotHashEqPlan *hp = &bundle->hash_eq_plans[hi];
        fprintf(out,
                "hash-eq-plan %u id=%u type=%u kind=%s action=%s eq_derive=%u hash_derive=%u "
                "eq_func=%u hash_func=%u evidence=0x%x reason=%s\n",
                hi, hp->hash_eq_id, hp->type_key, xg_hash_eq_kind_name(hp->kind),
                hash_eq_action_name(hp->action), hp->eq_derive_id, hp->hash_derive_id,
                hp->eq_func_id, hp->hash_func_id, hp->evidence,
                map_unproven_reason_name(hp->unproven_reason));
    }

    for (uint32_t ki = 0; ki < bundle->nkey_access_plans; ki++) {
        const XaotKeyAccessPlan *kp = &bundle->key_access_plans[ki];
        fprintf(out,
                "key-access-plan %u id=%u func=%u span=%u ordinal=%u container=%s op=%s "
                "shape=%u action=%s receiver_type=%u key_type=%u value_type=%u key_const=%u "
                "prehash=%016" PRIx64 " evidence=0x%x reason=%s\n",
                ki, kp->access_id, kp->owner_func_id, kp->source_span_id, kp->body_ordinal,
                xg_map_container_kind_name(kp->container_kind), xg_key_access_op_name(kp->op),
                kp->receiver_shape_id, key_access_action_name(kp->action), kp->receiver_type_key,
                kp->key_type_key, kp->value_type_key, kp->key_const_id, kp->key_prehash,
                kp->evidence, map_unproven_reason_name(kp->unproven_reason));
    }

    for (uint32_t si = 0; si < bundle->nsequence_access_plans; si++) {
        const XaotSequenceAccessPlan *sp = &bundle->sequence_access_plans[si];
        fprintf(out,
                "sequence-access-plan %u id=%u func=%u span=%u ordinal=%u kind=%s access=%s "
                "action=%s receiver_type=%u elem_type=%u index_expr=%u length_expr=%u evidence=",
                si, sp->access_id, sp->owner_func_id, sp->source_span_id, sp->body_ordinal,
                xg_sequence_kind_name(sp->sequence_kind),
                xg_sequence_access_kind_name(sp->access_kind),
                sequence_access_action_name(sp->action), sp->receiver_type_key, sp->elem_type_key,
                sp->index_expr_id, sp->length_expr_id);
        print_sequence_evidence_bits(out, sp->evidence);
        fprintf(out, " reason=%s\n", sequence_unproven_reason_name(sp->unproven_reason));
    }

    for (uint32_t ci = 0; ci < bundle->ncapacity_plans; ci++) {
        const XaotCapacityPlan *cp = &bundle->capacity_plans[ci];
        fprintf(out,
                "capacity-plan %u id=%u func=%u span=%u ordinal=%u kind=%s op=%s action=%s "
                "receiver_type=%u elem_type=%u count_expr=%u loop=%u evidence=",
                ci, cp->op_id, cp->owner_func_id, cp->source_span_id, cp->body_ordinal,
                xg_sequence_kind_name(cp->sequence_kind), xg_capacity_op_kind_name(cp->op_kind),
                capacity_action_name(cp->action), cp->receiver_type_key, cp->elem_type_key,
                cp->count_expr_id, cp->loop_id);
        print_capacity_evidence_bits(out, cp->evidence);
        fprintf(out, " reason=%s\n", capacity_unproven_reason_name(cp->unproven_reason));
    }

    for (uint32_t bi = 0; bi < bundle->nbulk_plans; bi++) {
        const XaotBulkPlan *bp = &bundle->bulk_plans[bi];
        fprintf(out,
                "bulk-plan %u id=%u func=%u span=%u ordinal=%u op=%s action=%s elem_type=%u "
                "src_type=%u dst_type=%u length_expr=%u evidence=",
                bi, bp->op_id, bp->owner_func_id, bp->source_span_id, bp->body_ordinal,
                xg_bulk_op_kind_name(bp->op_kind), bulk_action_name(bp->action), bp->elem_type_key,
                bp->src_type_key, bp->dst_type_key, bp->length_expr_id);
        print_bulk_evidence_bits(out, bp->evidence);
        fprintf(out, " reason=%s\n", bulk_unproven_reason_name(bp->unproven_reason));
    }

    for (uint32_t ei = 0; ei < bundle->nencoding_plans; ei++) {
        const XaotEncodingPlan *ep = &bundle->encoding_plans[ei];
        fprintf(out,
                "encoding-plan %u id=%u func=%u span=%u ordinal=%u op=%s action=%s "
                "input_type=%u output_type=%u evidence=",
                ei, ep->op_id, ep->owner_func_id, ep->source_span_id, ep->body_ordinal,
                xg_encoding_op_kind_name(ep->op_kind), encoding_action_name(ep->action),
                ep->input_type_key, ep->output_type_key);
        print_encoding_evidence_bits(out, ep->evidence);
        fprintf(out, " reason=%s\n", encoding_unproven_reason_name(ep->unproven_reason));
    }

    for (uint32_t mi = 0; mi < bundle->nmetadata_plans; mi++) {
        const XaotMetadataReachabilityPlan *mp = &bundle->metadata_plans[mi];
        fprintf(out, "metadata %u name=%s bodies=%u decls=%u action=%s evidence=0x%x reason=%s\n",
                mi, xg_metadata_name(mp->metadata), mp->body_count, mp->decl_count,
                capability_action_name(mp->profile_action), mp->evidence,
                metadata_unproven_reason_name(mp->unproven_reason));
    }

    for (uint32_t ci = 0; ci < bundle->ncapability_plans; ci++) {
        const XaotCapabilityPlan *cp = &bundle->capability_plans[ci];
        fprintf(out,
                "capability %u name=%s bodies=%u action=%s transfers=%u evidence=0x%x "
                "reason=%s\n",
                ci, xg_capability_name(cp->capability), cp->body_count,
                capability_action_name(cp->profile_action), cp->transfer_count, cp->evidence,
                capability_unproven_reason_name(cp->unproven_reason));
    }

    for (uint32_t si = 0; si < bundle->nstatic_data_plans; si++) {
        const XaotStaticDataPlan *sp = &bundle->static_data_plans[si];
        fprintf(out,
                "static-data %u name=%s bodies=%u action=%s section=%s align=%u "
                "type_hash=%016" PRIx64 " data_hash=%016" PRIx64 " evidence=0x%x reason=%s\n",
                si, xg_static_data_name(sp->static_data), sp->body_count,
                static_data_action_name(sp->action), static_data_section_name(sp->section),
                sp->align, sp->type_hash, sp->data_hash, sp->evidence,
                static_data_unproven_reason_name(sp->unproven_reason));
    }

    for (uint32_t li = 0; li < bundle->nlink_dependency_plans; li++) {
        const XaotLinkDependencyPlan *lp = &bundle->link_dependency_plans[li];
        fprintf(out,
                "link-dependency %u id=%u kind=%s name_id=%u name=%s evidence=0x%x reason=%u\n", li,
                lp->link_id, xg_link_dependency_kind_name(lp->kind), lp->name_id, lp->name,
                lp->evidence, (unsigned) lp->unproven_reason);
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
        fprintf(out,
                "enum-scalar-plan %u enum=%u action=%s evidence=0x%x payload_cap=%u "
                "c_type=%s\n",
                ei, ei, enum_scalar_action_name(ep->scalar_action), ep->scalar_evidence,
                (unsigned) ep->scalar_payload_cap, safe_str(ep->c_type));
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
        fprintf(out, "fn-attr %u func=%s body=%u attr=%s effect=0x%x escape=0x%x evidence=0x%x\n",
                ai, safe_str(ap->func ? ap->func->name : NULL), ap->body_func_id,
                (ap->flags & XAOT_FN_ATTR_CONST) ? "const" : "pure", ap->body_effect_bits,
                ap->body_escape_bits, ap->evidence);
    }

    for (uint32_t ai = 0; ai < bundle->nbounds_plans; ai++) {
        const XaotBoundsPlan *bp = &bundle->bounds_plans[ai];
        char access_buf[32];
        const char *op_name =
            bp->access && bp->access->op == XI_INDEX_SET ? "index_set" : "index_get";
        value_ref(access_buf, sizeof(access_buf), bp->access);
        if (bp->evidence != 0) {
            fprintf(out, "bounds %u func=%s access=%s op=%s evidence=", ai,
                    safe_str(bp->func ? bp->func->name : NULL), access_buf, op_name);
            print_bounds_evidence_bits(out, bp->evidence);
        } else {
            static const char *const reason_names[] = {"none", "no_guard", "index_range",
                                                       "len_mismatch", "clobber"};
            const char *reason =
                bp->unproven_reason < 5 ? reason_names[bp->unproven_reason] : "unknown";
            fprintf(out, "bounds-unproven %u func=%s access=%s op=%s reason=%s", ai,
                    safe_str(bp->func ? bp->func->name : NULL), access_buf, op_name, reason);
        }
        fprintf(out, " body=%u effect=0x%x escape=0x%x body_evidence=0x%x\n", bp->body_func_id,
                bp->body_effect_bits, bp->body_escape_bits, bp->body_evidence);
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
        } else {
            fprintf(out, "span-access-unproven %u func=%s value=%s kind=%s evidence=", ai,
                    safe_str(sp->func ? sp->func->name : NULL), value_buf,
                    span_access_kind_name(sp->kind));
            print_span_access_bits(out, sp->evidence, true);
            fprintf(out, " reason=%s", span_access_reason_name(sp->unproven_reason));
        }
        fprintf(out, " body=%u effect=0x%x escape=0x%x body_evidence=0x%x\n", sp->body_func_id,
                sp->body_effect_bits, sp->body_escape_bits, sp->body_evidence);
    }

    for (uint32_t ai = 0; ai < bundle->nalias_plans; ai++) {
        const XaotAliasPlan *ap = &bundle->alias_plans[ai];
        char value_buf[32];
        static const char *const kind_names[] = {"none", "unique_data", "unique_recv",
                                                 "unique_param"};
        value_ref(value_buf, sizeof(value_buf), ap->value);
        fprintf(out,
                "alias %u func=%s value=%s kind=%s evidence=%s%s%s%s body=%u effect=0x%x "
                "escape=0x%x body_evidence=0x%x\n",
                ai, safe_str(ap->func ? ap->func->name : NULL), value_buf,
                ap->kind < 4 ? kind_names[ap->kind] : "unknown",
                (ap->evidence & XAOT_ALIAS_EV_FRESH_ALLOC) ? "fresh" : "",
                (ap->evidence & XAOT_ALIAS_EV_ALL_ACCESS_RAW) ? "+raw" : "",
                (ap->evidence & XAOT_ALIAS_EV_USE_WHITELIST) ? "+whitelist" : "",
                (ap->evidence & XAOT_ALIAS_EV_SOLE_CACHE) ? "+sole" : "", ap->body_func_id,
                ap->body_effect_bits, ap->body_escape_bits, ap->body_evidence);
    }

    for (uint32_t ai = 0; ai < bundle->nallocation_plans; ai++) {
        const XaotAllocationPlan *ap = &bundle->allocation_plans[ai];
        char value_buf[32];
        value_ref(value_buf, sizeof(value_buf), ap->value);
        fprintf(out,
                "allocation-plan %u func=%s value=%s action=%s original=%s escape=%u "
                "evidence=0x%x body=%u effect=0x%x body_escape=0x%x body_evidence=0x%x\n",
                ai, safe_str(ap->func ? ap->func->name : NULL), value_buf,
                allocation_action_name(ap->action), xi_op_name(ap->original_op),
                (unsigned) ap->escape, ap->evidence, ap->body_func_id, ap->body_effect_bits,
                ap->body_escape_bits, ap->body_evidence);
    }

    for (uint32_t ci = 0; ci < bundle->nclosure_plans; ci++) {
        const XaotClosurePlan *cp = &bundle->closure_plans[ci];
        char value_buf[32];
        value_ref(value_buf, sizeof(value_buf), cp->value);
        fprintf(out, "closure %u func=%s value=%s target=%s captures=%u repr=%s evidence=", ci,
                safe_str(cp->func ? cp->func->name : NULL), value_buf,
                safe_str(cp->target_func ? cp->target_func->name : NULL),
                (unsigned) cp->capture_count, closure_representation_name(cp->representation));
        print_closure_evidence_bits(out, cp->evidence);
        fprintf(out, " reason=%s\n", closure_unproven_reason_name(cp->unproven_reason));
    }

    for (uint32_t ti = 0; ti < bundle->ntransfer_plans; ti++) {
        const XaotTransferPlan *tp = &bundle->transfer_plans[ti];
        char site_buf[32];
        char value_buf[32];
        value_ref(site_buf, sizeof(site_buf), tp->site);
        value_ref(value_buf, sizeof(value_buf), tp->value);
        fprintf(out,
                "transfer %u func=%s site=%s kind=%s index=%u value=%s mode=%s action=%s "
                "type-key=%016" PRIx64 " evidence=",
                ti, safe_str(tp->func ? tp->func->name : NULL), site_buf,
                transfer_site_kind_name(tp->site_kind), (unsigned) tp->transfer_index, value_buf,
                transfer_mode_name(tp->mode), transfer_action_name(tp->action),
                tp->value_type_key.fingerprint);
        print_transfer_evidence_bits(out, tp->evidence);
        fprintf(out, " reason=%s\n", transfer_unproven_reason_name(tp->unproven_reason));
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
