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

static bool xaot_bundle_add_dispatch_target_case(XaotBundle *bundle, XgCallsiteId callsite_id,
                                                 XgClassId receiver_class_id, XgMethodId method_id,
                                                 uint32_t evidence) {
    XaotDispatchTargetCase *target;
    if (!bundle || callsite_id == XG_NO_ID || receiver_class_id == XG_NO_ID ||
        method_id == XG_NO_ID)
        return false;
    if (!reserve_plan_array((void **) &bundle->dispatch_target_cases,
                            &bundle->dispatch_target_case_cap, bundle->ndispatch_target_cases + 1,
                            sizeof(XaotDispatchTargetCase), 8))
        return false;
    target = &bundle->dispatch_target_cases[bundle->ndispatch_target_cases++];
    memset(target, 0, sizeof(*target));
    target->callsite_id = callsite_id;
    target->receiver_class_id = receiver_class_id;
    target->method_id = method_id;
    target->evidence = evidence;
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
                          XG_CLASS_INFERRED_FINAL | XG_CLASS_NATIVE | XG_CLASS_RUNTIME_ONLY);
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
            for (uint32_t i = 0; i < ev->ninterface_impls; i++) {
                const XgInterfaceImplSummary *impl = &ev->interface_impls[i];
                const XgMethodSummary *target_method;
                if (impl->interface_id != call->receiver_static_interface_id)
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
        }
    }

    if (kind == XAOT_DISPATCH_DIRECT && method) {
        target_start = bundle->ndispatch_target_cases + 1;
        if (!xaot_bundle_add_dispatch_target_case(bundle, call->callsite_id,
                                                  call->receiver_static_class_id, method->method_id,
                                                  evidence))
            return false;
        target_count = 1;
    } else if ((kind == XAOT_DISPATCH_DIRECT || kind == XAOT_DISPATCH_TYPE_SWITCH) &&
               call->kind == XG_CALL_INTERFACE) {
        target_start = bundle->ndispatch_target_cases + 1;
        for (uint32_t i = 0; i < ev->ninterface_impls; i++) {
            const XgInterfaceImplSummary *impl = &ev->interface_impls[i];
            const XgMethodSummary *target_method;
            if (impl->interface_id != call->receiver_static_interface_id)
                continue;
            target_method = xg_evidence_find_method_by_signature_in_hierarchy(
                ev, impl->implementor_class_id, call->method_name_id, call->method_signature_key);
            if (!target_method || !xaot_bundle_add_dispatch_target_case(
                                      bundle, call->callsite_id, impl->implementor_class_id,
                                      target_method->method_id, evidence))
                return false;
            target_count++;
        }
    }

    plan = &bundle->method_dispatch_plans[bundle->nmethod_dispatch_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->callsite_id = call->callsite_id;
    plan->source_span_id = call->source_span_id;
    plan->method_id = method ? method->method_id : call->method_id;
    plan->receiver_static_class_id = call->receiver_static_class_id;
    plan->kind = kind;
    plan->dispatch_slot = UINT32_MAX;
    plan->target_start = target_start;
    plan->target_count = target_count;
    plan->evidence = kind == XAOT_DISPATCH_RUNTIME_FALLBACK ? 0 : evidence;
    plan->unproven_reason = reason;
    return true;
}

static bool xaot_bundle_add_interface_use_plan(XaotBundle *bundle,
                                               const XgInterfaceImplSummary *impl) {
    XaotInterfaceUsePlan *plan;
    if (!bundle || !impl)
        return false;
    if (!reserve_plan_array((void **) &bundle->interface_use_plans, &bundle->interface_use_plan_cap,
                            bundle->ninterface_use_plans + 1, sizeof(XaotInterfaceUsePlan), 8))
        return false;
    plan = &bundle->interface_use_plans[bundle->ninterface_use_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->interface_id = impl->interface_id;
    plan->implementor_class_id = impl->implementor_class_id;
    plan->use_site_id = XG_NO_ID;
    plan->reason = XAOT_INTERFACE_USE_REASON_IMPLEMENTS;
    plan->flags = 0;
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
            if (bit == XG_METADATA_DERIVE && (evidence->decls[di].flags & XG_DECL_DERIVE) != 0)
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

static uint32_t xaot_static_data_action(uint32_t profile, uint32_t static_data) {
    if (profile == XG_BUILD_FREESTANDING && static_data == XG_STATIC_DATA_RUNTIME_INIT)
        return XAOT_STATIC_DATA_ACTION_REJECT;
    if (static_data == XG_STATIC_DATA_RUNTIME_INIT)
        return XAOT_STATIC_DATA_ACTION_RUNTIME_INIT;
    return XAOT_STATIC_DATA_ACTION_MATERIALIZE;
}

static bool xaot_bundle_add_static_data_plan(XaotBundle *bundle, uint32_t static_data,
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
    plan->action = xaot_static_data_action(bundle->global_evidence_plan.profile, static_data);
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
        if (!xaot_bundle_add_static_data_plan(bundle, bit, body_count))
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
    return xaot_bundle_populate_global_lowered_plans(bundle, evidence);
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

static const char *static_data_action_name(uint32_t action) {
    switch ((XaotStaticDataAction) action) {
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
                " decls=%u classes=%u methods=%u interface_impls=%u bodies=%u callsites=%u\n",
                xg_build_profile_name(bundle->global_evidence_plan.profile),
                bundle->global_evidence_plan.evidence_hash, ev->ndecls, ev->nclasses, ev->nmethods,
                ev->ninterface_impls, ev->nbodies, ev->ncallsites);
    } else {
        fprintf(out, "global-evidence profile=none hash=0000000000000000 decls=0 classes=0 "
                     "methods=0 interface_impls=0 bodies=0 callsites=0\n");
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
                    "method-dispatch %u callsite=%u span=%u kind=%s method=%u recv_class=%u "
                    "slot=- targets=%u+%u evidence=0x%x reason=%s\n",
                    di, dp->callsite_id, dp->source_span_id, dispatch_kind_name(dp->kind),
                    dp->method_id, dp->receiver_static_class_id, dp->target_start,
                    (unsigned) dp->target_count, dp->evidence,
                    dispatch_unproven_reason_name(dp->unproven_reason));
        } else {
            fprintf(out,
                    "method-dispatch %u callsite=%u span=%u kind=%s method=%u recv_class=%u "
                    "slot=%u targets=%u+%u evidence=0x%x reason=%s\n",
                    di, dp->callsite_id, dp->source_span_id, dispatch_kind_name(dp->kind),
                    dp->method_id, dp->receiver_static_class_id, dp->dispatch_slot,
                    dp->target_start, (unsigned) dp->target_count, dp->evidence,
                    dispatch_unproven_reason_name(dp->unproven_reason));
        }
    }

    for (uint32_t ti = 0; ti < bundle->ndispatch_target_cases; ti++) {
        const XaotDispatchTargetCase *tc = &bundle->dispatch_target_cases[ti];
        fprintf(out, "dispatch-target %u callsite=%u recv_class=%u method=%u evidence=0x%x\n", ti,
                tc->callsite_id, tc->receiver_class_id, tc->method_id, tc->evidence);
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
        fprintf(out, "static-data %u name=%s bodies=%u action=%s evidence=0x%x reason=%s\n", si,
                xg_static_data_name(sp->static_data), sp->body_count,
                static_data_action_name(sp->action), sp->evidence,
                static_data_unproven_reason_name(sp->unproven_reason));
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
