/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_ownership_audit.c - Manifest-driven executed-path ownership audit heap
 */

#include "xr_ownership_audit.h"
#include "../../base/xmalloc.h"
#include <limits.h>
#include <stdatomic.h>
#include <string.h>

#define XR_AUDIT_TRANSITION_FLAGS_ALL                                                        \
    (XR_OWN_AUDIT_TRANSITION_TERMINAL | XR_OWN_AUDIT_TRANSITION_OPENS_INSTANCE |             \
     XR_OWN_AUDIT_TRANSITION_CHANGES_DOMAIN)

typedef struct XrAuditOwnerManifest {
    XrStableId owner_id;
    XrStableId layout_id;
    XrStableId allocation_site_id;
    XrStableId frame_id;
    XrStableId generation_id;
    XrStableId destructor_id;
    XrStableId initial_domain_contract_id;
    XrFingerprint premise_fingerprint;
    uint32_t allowed_semantic_domains;
    uint32_t allowed_materializations;
    int32_t initial_logical_balance;
    uint32_t flags;
    uint8_t initial_state;
    uint8_t initial_semantic_domain;
    uint8_t initial_materialization;
} XrAuditOwnerManifest;

typedef struct XrAuditObject {
    XrOwnershipAuditObjectKey key;
    XrStableId layout_id;
    XrStableId allocation_site_id;
    XrStableId frame_id;
    XrStableId generation_id;
    XrStableId destructor_id;
    XrFingerprint premise_fingerprint;
    XrRuntimeDomainIdentity domain;
    uint32_t allowed_semantic_domains;
    uint32_t allowed_materializations;
    uint32_t flags;
    int32_t logical_balance;
    int32_t last_physical_rc;
    uint32_t active_loans;
    uint8_t logical_state;
    uint8_t last_physical_mode;
    bool physical_seen;
    bool terminal;
} XrAuditObject;

typedef struct XrAuditLoan {
    XrStableId loan_id;
    XrOwnershipAuditObjectKey object;
    bool active;
} XrAuditLoan;

typedef struct XrAuditGeneration {
    XrStableId generation_id;
    uint32_t pin_count;
} XrAuditGeneration;

struct XrOwnershipAudit {
    XrAuditOwnerManifest *owners;
    XrOwnershipAuditTransitionManifest *transitions;
    XrAuditObject *objects;
    XrAuditLoan *loans;
    XrAuditGeneration *generations;
    XrOwnershipAuditEvent *events;
    size_t owner_count;
    size_t transition_count;
    size_t object_count;
    size_t loan_count;
    size_t generation_count;
    size_t event_count;
    size_t owner_capacity;
    size_t transition_capacity;
    size_t object_capacity;
    size_t loan_capacity;
    size_t generation_capacity;
    size_t event_capacity;
    atomic_int status;
    XrOwnershipAuditEvent failed_event;
    atomic_flag gate;
    bool has_failed_event;
    bool finished;
};

static bool id_is_zero(XrStableId id) {
    static const XrStableId zero = {{0}};
    return memcmp(id.bytes, zero.bytes, sizeof(id.bytes)) == 0;
}

static bool id_equal(XrStableId left, XrStableId right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

static bool fingerprint_equal(XrFingerprint left, XrFingerprint right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

static bool fingerprint_is_zero(XrFingerprint fingerprint) {
    static const XrFingerprint zero = {{0}};
    return fingerprint_equal(fingerprint, zero);
}

static bool domain_equal(XrRuntimeDomainIdentity left, XrRuntimeDomainIdentity right) {
    return id_equal(left.contract_id, right.contract_id) &&
           left.instance_id == right.instance_id &&
           left.semantic_domain == right.semantic_domain &&
           left.materialization == right.materialization;
}

static bool domain_is_zero(XrRuntimeDomainIdentity domain) {
    return id_is_zero(domain.contract_id) && domain.instance_id == 0 &&
           domain.semantic_domain == 0 && domain.materialization == 0;
}

static bool domain_matches_manifest(XrRuntimeDomainIdentity domain, XrStableId contract_id,
                                    uint8_t semantic_domain, uint8_t materialization) {
    return xr_runtime_domain_identity_valid(domain) &&
           id_equal(domain.contract_id, contract_id) &&
           domain.semantic_domain == semantic_domain &&
           domain.materialization == materialization;
}

static bool object_key_valid(XrOwnershipAuditObjectKey key) {
    return !id_is_zero(key.owner_id) && !id_is_zero(key.invocation_id) &&
           key.activation_epoch != 0;
}

static bool object_key_is_zero(XrOwnershipAuditObjectKey key) {
    return id_is_zero(key.owner_id) && id_is_zero(key.invocation_id) &&
           key.activation_epoch == 0;
}

static bool object_key_equal(XrOwnershipAuditObjectKey left,
                             XrOwnershipAuditObjectKey right) {
    return id_equal(left.owner_id, right.owner_id) &&
           id_equal(left.invocation_id, right.invocation_id) &&
           left.activation_epoch == right.activation_epoch;
}

static bool checked_bytes(size_t count, size_t element_size) {
    return element_size != 0 && count <= SIZE_MAX / element_size;
}

static void *allocate_table(size_t count, size_t element_size) {
    return count != 0 && checked_bytes(count, element_size) ? xr_calloc(count, element_size)
                                                            : NULL;
}

static XrOwnershipAuditStatus status_load(const XrOwnershipAudit *audit) {
    return (XrOwnershipAuditStatus) atomic_load_explicit(&audit->status,
                                                         memory_order_acquire);
}

static bool enter_audit(XrOwnershipAudit *audit) {
    if (!atomic_flag_test_and_set_explicit(&audit->gate, memory_order_acquire))
        return true;
    int expected = XR_OWN_AUDIT_OK;
    atomic_compare_exchange_strong_explicit(&audit->status, &expected,
                                            XR_OWN_AUDIT_REENTRANT,
                                            memory_order_acq_rel, memory_order_acquire);
    return false;
}

static void leave_audit(XrOwnershipAudit *audit) {
    atomic_flag_clear_explicit(&audit->gate, memory_order_release);
}

static XrOwnershipAuditStatus fail(XrOwnershipAudit *audit, XrOwnershipAuditStatus status,
                                   const XrOwnershipAuditEvent *event) {
    int expected = XR_OWN_AUDIT_OK;
    atomic_compare_exchange_strong_explicit(&audit->status, &expected, status,
                                            memory_order_acq_rel, memory_order_acquire);
    if (event && !audit->has_failed_event) {
        audit->failed_event = *event;
        audit->has_failed_event = true;
    }
    return status_load(audit);
}

static XrAuditOwnerManifest *find_owner(XrOwnershipAudit *audit, XrStableId owner_id) {
    for (size_t i = 0; i < audit->owner_count; i++) {
        if (id_equal(audit->owners[i].owner_id, owner_id))
            return &audit->owners[i];
    }
    return NULL;
}

static XrOwnershipAuditTransitionManifest *find_transition(XrOwnershipAudit *audit,
                                                           XrStableId transition_id) {
    for (size_t i = 0; i < audit->transition_count; i++) {
        if (id_equal(audit->transitions[i].transition_id, transition_id))
            return &audit->transitions[i];
    }
    return NULL;
}

static XrAuditObject *find_object(XrOwnershipAudit *audit,
                                  XrOwnershipAuditObjectKey key) {
    for (size_t i = 0; i < audit->object_count; i++) {
        if (object_key_equal(audit->objects[i].key, key))
            return &audit->objects[i];
    }
    return NULL;
}

static XrAuditLoan *find_active_loan(XrOwnershipAudit *audit,
                                    XrOwnershipAuditObjectKey object, XrStableId loan_id) {
    for (size_t i = 0; i < audit->loan_count; i++) {
        if (audit->loans[i].active && object_key_equal(audit->loans[i].object, object) &&
            id_equal(audit->loans[i].loan_id, loan_id))
            return &audit->loans[i];
    }
    return NULL;
}

static XrAuditGeneration *find_generation(XrOwnershipAudit *audit,
                                          XrStableId generation_id) {
    for (size_t i = 0; i < audit->generation_count; i++) {
        if (id_equal(audit->generations[i].generation_id, generation_id))
            return &audit->generations[i];
    }
    return NULL;
}

XrOwnershipAudit *xr_ownership_audit_create(XrOwnershipAuditConfig config,
                                            XrOwnershipAuditStatus *status) {
    if (status)
        *status = XR_OWN_AUDIT_INVALID_ARGUMENT;
    if (config.max_owner_manifests == 0 || config.max_transition_manifests == 0 ||
        config.max_dynamic_instances == 0 || config.max_events == 0 ||
        config.max_loans == 0 ||
        config.max_generations == 0)
        return NULL;

    XrOwnershipAudit *audit = (XrOwnershipAudit *) xr_calloc(1, sizeof(*audit));
    if (!audit) {
        if (status)
            *status = XR_OWN_AUDIT_OUT_OF_MEMORY;
        return NULL;
    }
    audit->owners =
        (XrAuditOwnerManifest *) allocate_table(config.max_owner_manifests,
                                                sizeof(*audit->owners));
    audit->transitions = (XrOwnershipAuditTransitionManifest *) allocate_table(
        config.max_transition_manifests, sizeof(*audit->transitions));
    audit->objects = (XrAuditObject *) allocate_table(config.max_dynamic_instances,
                                                      sizeof(*audit->objects));
    audit->events =
        (XrOwnershipAuditEvent *) allocate_table(config.max_events, sizeof(*audit->events));
    audit->loans = (XrAuditLoan *) allocate_table(config.max_loans, sizeof(*audit->loans));
    audit->generations = (XrAuditGeneration *) allocate_table(config.max_generations,
                                                              sizeof(*audit->generations));
    if (!audit->owners || !audit->transitions || !audit->objects || !audit->events ||
        !audit->loans || !audit->generations) {
        xr_ownership_audit_destroy(audit);
        if (status)
            *status = XR_OWN_AUDIT_OUT_OF_MEMORY;
        return NULL;
    }
    audit->owner_capacity = config.max_owner_manifests;
    audit->transition_capacity = config.max_transition_manifests;
    audit->object_capacity = config.max_dynamic_instances;
    audit->event_capacity = config.max_events;
    audit->loan_capacity = config.max_loans;
    audit->generation_capacity = config.max_generations;
    atomic_init(&audit->status, XR_OWN_AUDIT_OK);
    atomic_flag_clear(&audit->gate);
    if (status)
        *status = XR_OWN_AUDIT_OK;
    return audit;
}

void xr_ownership_audit_destroy(XrOwnershipAudit *audit) {
    if (!audit)
        return;
    xr_free(audit->owners);
    xr_free(audit->transitions);
    xr_free(audit->objects);
    xr_free(audit->events);
    xr_free(audit->loans);
    xr_free(audit->generations);
    xr_free(audit);
}

static XrOwnershipAuditStatus validate_owner_manifest(
    const XrOwnershipAuditOwnerManifest *manifest) {
    if (!manifest || !manifest->descriptor || !manifest->extent ||
        id_is_zero(manifest->owner_id) || id_is_zero(manifest->allocation_site_id) ||
        id_is_zero(manifest->frame_id) || id_is_zero(manifest->initial_domain_contract_id) ||
        fingerprint_is_zero(manifest->premise_fingerprint) ||
        manifest->initial_state >= XR_OWN_STATE_COUNT || manifest->initial_logical_balance < 0 ||
        (manifest->flags & ~XR_OWN_AUDIT_REQUIRE_GENERATION_PIN) != 0)
        return XR_OWN_AUDIT_INVALID_ARGUMENT;
    XrRuntimeDomainIdentity domain = {
        .contract_id = manifest->initial_domain_contract_id,
        .instance_id = UINT32_C(1),
        .semantic_domain = manifest->initial_semantic_domain,
        .materialization = manifest->initial_materialization,
    };
    if (xr_runtime_layout_descriptor_verify(manifest->descriptor, manifest->extent) !=
            XR_RUNTIME_ABI_OK ||
        !xr_runtime_layout_allows_domain(manifest->descriptor, domain) ||
        !id_equal(manifest->destructor_id, manifest->descriptor->destructor_id))
        return XR_OWN_AUDIT_INVALID_DESCRIPTOR;
    if ((manifest->flags & XR_OWN_AUDIT_REQUIRE_GENERATION_PIN) != 0 &&
        id_is_zero(manifest->generation_id))
        return XR_OWN_AUDIT_INVALID_ARGUMENT;
    return XR_OWN_AUDIT_OK;
}

XrOwnershipAuditStatus xr_ownership_audit_register_owner(
    XrOwnershipAudit *audit, const XrOwnershipAuditOwnerManifest *manifest) {
    if (!audit)
        return XR_OWN_AUDIT_INVALID_ARGUMENT;
    if (!enter_audit(audit))
        return status_load(audit);
    XrOwnershipAuditStatus result = status_load(audit);
    if (result != XR_OWN_AUDIT_OK)
        goto done;
    if (audit->finished || audit->event_count != 0) {
        result = fail(audit, XR_OWN_AUDIT_INVALID_TRANSITION, NULL);
        goto done;
    }
    result = validate_owner_manifest(manifest);
    if (result != XR_OWN_AUDIT_OK) {
        result = fail(audit, result, NULL);
        goto done;
    }
    if (find_owner(audit, manifest->owner_id)) {
        result = fail(audit, XR_OWN_AUDIT_DUPLICATE_OWNER, NULL);
        goto done;
    }
    if (audit->owner_count == audit->owner_capacity) {
        result = fail(audit, XR_OWN_AUDIT_CAPACITY_EXCEEDED, NULL);
        goto done;
    }
    audit->owners[audit->owner_count++] = (XrAuditOwnerManifest) {
        .owner_id = manifest->owner_id,
        .layout_id = manifest->descriptor->layout_id,
        .allocation_site_id = manifest->allocation_site_id,
        .frame_id = manifest->frame_id,
        .generation_id = manifest->generation_id,
        .destructor_id = manifest->destructor_id,
        .initial_domain_contract_id = manifest->initial_domain_contract_id,
        .premise_fingerprint = manifest->premise_fingerprint,
        .allowed_semantic_domains = manifest->descriptor->allowed_semantic_domains,
        .allowed_materializations = manifest->descriptor->allowed_materializations,
        .initial_logical_balance = manifest->initial_logical_balance,
        .flags = manifest->flags,
        .initial_state = manifest->initial_state,
        .initial_semantic_domain = manifest->initial_semantic_domain,
        .initial_materialization = manifest->initial_materialization,
    };
done:
    leave_audit(audit);
    return result;
}

static bool transition_has_domain_change(const XrOwnershipAuditTransitionManifest *manifest) {
    return (manifest->flags & XR_OWN_AUDIT_TRANSITION_CHANGES_DOMAIN) != 0;
}

static XrOwnershipAuditStatus validate_transition_manifest(
    const XrOwnershipAuditTransitionManifest *manifest) {
    if (!manifest || id_is_zero(manifest->transition_id) ||
        id_is_zero(manifest->operation_id) || manifest->kind >= XR_OWN_EVENT_COUNT ||
        manifest->program_point >= XR_OWN_POINT_COUNT ||
        (manifest->flags & ~XR_AUDIT_TRANSITION_FLAGS_ALL) != 0)
        return XR_OWN_AUDIT_INVALID_ARGUMENT;
    bool pin = manifest->kind == XR_OWN_EVENT_PIN || manifest->kind == XR_OWN_EVENT_UNPIN;
    if (pin) {
        if (!id_is_zero(manifest->owner_id) || id_is_zero(manifest->generation_id) ||
            !id_is_zero(manifest->exit_id) || manifest->flags != 0 ||
            manifest->state_before_mask != 0 || manifest->logical_delta != 0 ||
            manifest->state_after != 0 || !id_is_zero(manifest->next_domain_contract_id) ||
            manifest->next_semantic_domain != 0 || manifest->next_materialization != 0)
            return XR_OWN_AUDIT_INVALID_ARGUMENT;
        return XR_OWN_AUDIT_OK;
    }
    if (id_is_zero(manifest->owner_id) || !id_is_zero(manifest->generation_id) ||
        manifest->state_before_mask == 0 ||
        (manifest->state_before_mask & ~XR_OWN_STATE_MASK_ALL) != 0 ||
        manifest->state_after >= XR_OWN_STATE_COUNT)
        return XR_OWN_AUDIT_INVALID_ARGUMENT;
    if ((manifest->flags & XR_OWN_AUDIT_TRANSITION_TERMINAL) != 0 &&
        id_is_zero(manifest->exit_id))
        return XR_OWN_AUDIT_INVALID_ARGUMENT;
    if (transition_has_domain_change(manifest)) {
        XrRuntimeDomainIdentity domain = {
            .contract_id = manifest->next_domain_contract_id,
            .instance_id = UINT32_C(1),
            .semantic_domain = manifest->next_semantic_domain,
            .materialization = manifest->next_materialization,
        };
        if (!xr_runtime_domain_identity_valid(domain))
            return XR_OWN_AUDIT_INVALID_ARGUMENT;
    } else if (!id_is_zero(manifest->next_domain_contract_id) ||
               manifest->next_semantic_domain != 0 || manifest->next_materialization != 0) {
        return XR_OWN_AUDIT_INVALID_ARGUMENT;
    }
    return XR_OWN_AUDIT_OK;
}

XrOwnershipAuditStatus xr_ownership_audit_register_transition(
    XrOwnershipAudit *audit, const XrOwnershipAuditTransitionManifest *manifest) {
    if (!audit)
        return XR_OWN_AUDIT_INVALID_ARGUMENT;
    if (!enter_audit(audit))
        return status_load(audit);
    XrOwnershipAuditStatus result = status_load(audit);
    if (result != XR_OWN_AUDIT_OK)
        goto done;
    if (audit->finished || audit->event_count != 0) {
        result = fail(audit, XR_OWN_AUDIT_INVALID_TRANSITION, NULL);
        goto done;
    }
    result = validate_transition_manifest(manifest);
    if (result != XR_OWN_AUDIT_OK) {
        result = fail(audit, result, NULL);
        goto done;
    }
    if (find_transition(audit, manifest->transition_id)) {
        result = fail(audit, XR_OWN_AUDIT_DUPLICATE_TRANSITION, NULL);
        goto done;
    }
    if (audit->transition_count == audit->transition_capacity) {
        result = fail(audit, XR_OWN_AUDIT_CAPACITY_EXCEEDED, NULL);
        goto done;
    }
    audit->transitions[audit->transition_count++] = *manifest;
done:
    leave_audit(audit);
    return result;
}

static XrOwnershipAuditStatus check_generation_pin(XrOwnershipAudit *audit,
                                                   const XrAuditObject *object) {
    if ((object->flags & XR_OWN_AUDIT_REQUIRE_GENERATION_PIN) == 0)
        return XR_OWN_AUDIT_OK;
    XrAuditGeneration *generation = find_generation(audit, object->generation_id);
    return generation && generation->pin_count != 0 ? XR_OWN_AUDIT_OK
                                                     : XR_OWN_AUDIT_GENERATION_PIN_MISSING;
}

static bool pin_event_shape_valid(const XrOwnershipAuditEvent *event) {
    return object_key_is_zero(event->object) && id_is_zero(event->layout_id) &&
           id_is_zero(event->allocation_site_id) && id_is_zero(event->exit_id) &&
           id_is_zero(event->frame_id) && !id_is_zero(event->generation_id) &&
           id_is_zero(event->destructor_id) && id_is_zero(event->loan_id) &&
           fingerprint_is_zero(event->premise_fingerprint) && domain_is_zero(event->domain) &&
           domain_is_zero(event->next_domain) && event->physical_rc_before == 0 &&
           event->physical_rc_after == 0 && event->flags == 0 &&
           event->physical_rc_mode == XR_OWN_AUDIT_RC_NONE;
}

static XrOwnershipAuditStatus record_pin(
    XrOwnershipAudit *audit, const XrOwnershipAuditTransitionManifest *transition,
    const XrOwnershipAuditEvent *event) {
    if (!pin_event_shape_valid(event) || transition->kind != event->kind ||
        transition->program_point != event->program_point ||
        !id_equal(transition->operation_id, event->operation_id) ||
        !id_equal(transition->generation_id, event->generation_id))
        return XR_OWN_AUDIT_IDENTITY_MISMATCH;
    XrAuditGeneration *generation = find_generation(audit, event->generation_id);
    if (event->kind == XR_OWN_EVENT_PIN) {
        if (!generation) {
            if (audit->generation_count == audit->generation_capacity)
                return XR_OWN_AUDIT_CAPACITY_EXCEEDED;
            generation = &audit->generations[audit->generation_count++];
            generation->generation_id = event->generation_id;
        }
        if (generation->pin_count == UINT32_MAX)
            return XR_OWN_AUDIT_GENERATION_PIN_MISMATCH;
        generation->pin_count++;
        return XR_OWN_AUDIT_OK;
    }
    if (!generation || generation->pin_count == 0)
        return XR_OWN_AUDIT_GENERATION_PIN_MISMATCH;
    generation->pin_count--;
    return XR_OWN_AUDIT_OK;
}

static bool event_static_identity_valid(const XrAuditOwnerManifest *owner,
                                        const XrOwnershipAuditEvent *event) {
    return id_equal(owner->layout_id, event->layout_id) &&
           id_equal(owner->frame_id, event->frame_id) &&
           id_equal(owner->generation_id, event->generation_id) &&
           fingerprint_equal(owner->premise_fingerprint, event->premise_fingerprint);
}

static void initialize_object(XrAuditObject *object,
                              const XrAuditOwnerManifest *owner,
                              const XrOwnershipAuditEvent *event) {
    *object = (XrAuditObject) {
        .key = event->object,
        .layout_id = owner->layout_id,
        .allocation_site_id = owner->allocation_site_id,
        .frame_id = owner->frame_id,
        .generation_id = owner->generation_id,
        .destructor_id = owner->destructor_id,
        .premise_fingerprint = owner->premise_fingerprint,
        .domain = event->domain,
        .allowed_semantic_domains = owner->allowed_semantic_domains,
        .allowed_materializations = owner->allowed_materializations,
        .flags = owner->flags,
        .logical_balance = owner->initial_logical_balance,
        .logical_state = owner->initial_state,
    };
}

static bool domain_allowed(const XrAuditObject *object, XrRuntimeDomainIdentity domain) {
    return xr_runtime_domain_identity_valid(domain) &&
           (object->allowed_semantic_domains &
            XR_SEMANTIC_DOMAIN_MASK(domain.semantic_domain)) != 0 &&
           (object->allowed_materializations &
            XR_MATERIALIZATION_MASK(domain.materialization)) != 0;
}

static XrOwnershipAuditStatus validate_observation(
    const XrOwnershipAuditTransitionManifest *transition, const XrAuditObject *object,
    const XrOwnershipAuditEvent *event) {
    if (transition->kind != event->kind ||
        transition->program_point != event->program_point ||
        !id_equal(transition->owner_id, event->object.owner_id) ||
        !id_equal(transition->operation_id, event->operation_id))
        return XR_OWN_AUDIT_IDENTITY_MISMATCH;
    if (!id_equal(transition->exit_id, event->exit_id))
        return XR_OWN_AUDIT_EXIT_MISMATCH;
    if (!id_equal(object->layout_id, event->layout_id) ||
        !id_equal(object->frame_id, event->frame_id) ||
        !id_equal(object->generation_id, event->generation_id) ||
        !fingerprint_equal(object->premise_fingerprint, event->premise_fingerprint))
        return XR_OWN_AUDIT_IDENTITY_MISMATCH;
    if (!id_equal(object->allocation_site_id, event->allocation_site_id))
        return XR_OWN_AUDIT_ORIGIN_MISMATCH;
    if (!domain_equal(object->domain, event->domain))
        return XR_OWN_AUDIT_DOMAIN_MISMATCH;
    if (transition_has_domain_change(transition)) {
        if (!domain_matches_manifest(event->next_domain,
                                     transition->next_domain_contract_id,
                                     transition->next_semantic_domain,
                                     transition->next_materialization) ||
            !domain_allowed(object, event->next_domain))
            return XR_OWN_AUDIT_DOMAIN_MISMATCH;
    } else if (!domain_is_zero(event->next_domain)) {
        return XR_OWN_AUDIT_INVALID_ARGUMENT;
    }
    if ((transition->state_before_mask & XR_OWN_STATE_MASK(object->logical_state)) == 0)
        return XR_OWN_AUDIT_INVALID_TRANSITION;
    return XR_OWN_AUDIT_OK;
}

static XrOwnershipAuditStatus validate_event_fields(
    const XrOwnershipAuditTransitionManifest *transition, const XrAuditObject *object,
    const XrOwnershipAuditEvent *event) {
    if (event->kind != XR_OWN_EVENT_BORROW && event->kind != XR_OWN_EVENT_END_BORROW &&
        !id_is_zero(event->loan_id))
        return XR_OWN_AUDIT_LOAN_MISMATCH;
    if (event->kind == XR_OWN_EVENT_DESTROY) {
        if (!id_equal(object->destructor_id, event->destructor_id))
            return XR_OWN_AUDIT_DESTRUCTOR_MISMATCH;
    } else if (!id_is_zero(event->destructor_id)) {
        return XR_OWN_AUDIT_DESTRUCTOR_MISMATCH;
    }
    bool physical = (event->flags & XR_OWN_AUDIT_EVENT_PHYSICAL_RC) != 0;
    if ((event->flags & ~XR_OWN_AUDIT_EVENT_PHYSICAL_RC) != 0 ||
        (physical && event->kind != XR_OWN_EVENT_RETAIN &&
         event->kind != XR_OWN_EVENT_RELEASE))
        return XR_OWN_AUDIT_INVALID_ARGUMENT;
    if (!physical && (event->physical_rc_before != 0 || event->physical_rc_after != 0 ||
                      event->physical_rc_mode != XR_OWN_AUDIT_RC_NONE))
        return XR_OWN_AUDIT_INVALID_ARGUMENT;
    return XR_OWN_AUDIT_OK;
}

static XrOwnershipAuditStatus update_logical_state(
    XrAuditObject *object, const XrOwnershipAuditTransitionManifest *transition) {
    int32_t delta = transition->logical_delta;
    if ((delta > 0 && object->logical_balance > INT32_MAX - delta) ||
        (delta < 0 && object->logical_balance < -delta))
        return XR_OWN_AUDIT_INVALID_TRANSITION;
    object->logical_balance += delta;
    object->logical_state = transition->state_after;
    return XR_OWN_AUDIT_OK;
}

static XrOwnershipAuditStatus update_physical_rc(XrAuditObject *object,
                                                 const XrOwnershipAuditEvent *event) {
    if ((event->flags & XR_OWN_AUDIT_EVENT_PHYSICAL_RC) == 0)
        return XR_OWN_AUDIT_OK;
    if (event->physical_rc_mode != XR_OWN_AUDIT_RC_LOCAL &&
        event->physical_rc_mode != XR_OWN_AUDIT_RC_SHARED &&
        event->physical_rc_mode != XR_OWN_AUDIT_RC_STICKY)
        return XR_OWN_AUDIT_PHYSICAL_RC_MISMATCH;
    if (object->physical_seen && event->physical_rc_before != object->last_physical_rc)
        return XR_OWN_AUDIT_PHYSICAL_RC_MISMATCH;
    if (event->physical_rc_mode == XR_OWN_AUDIT_RC_STICKY) {
        if (event->physical_rc_before != event->physical_rc_after)
            return XR_OWN_AUDIT_PHYSICAL_RC_MISMATCH;
    } else if (event->kind == XR_OWN_EVENT_RETAIN) {
        if (event->physical_rc_before < 1 || event->physical_rc_before == INT32_MAX ||
            event->physical_rc_after != event->physical_rc_before + 1)
            return XR_OWN_AUDIT_PHYSICAL_RC_MISMATCH;
    } else if (event->physical_rc_before < 1 ||
               event->physical_rc_after != event->physical_rc_before - 1) {
        return XR_OWN_AUDIT_PHYSICAL_RC_MISMATCH;
    }
    object->physical_seen = true;
    object->last_physical_rc = event->physical_rc_after;
    object->last_physical_mode = event->physical_rc_mode;
    return XR_OWN_AUDIT_OK;
}

static XrOwnershipAuditStatus record_owner_event(
    XrOwnershipAudit *audit, const XrOwnershipAuditTransitionManifest *transition,
    const XrOwnershipAuditEvent *event) {
    if (!object_key_valid(event->object))
        return XR_OWN_AUDIT_INVALID_ARGUMENT;
    bool opens = (transition->flags & XR_OWN_AUDIT_TRANSITION_OPENS_INSTANCE) != 0;
    XrAuditObject *stored = find_object(audit, event->object);
    XrAuditObject candidate;
    if (opens) {
        if (stored)
            return XR_OWN_AUDIT_DUPLICATE_INSTANCE;
        XrAuditOwnerManifest *owner = find_owner(audit, event->object.owner_id);
        if (!owner)
            return XR_OWN_AUDIT_UNKNOWN_OWNER;
        if (!event_static_identity_valid(owner, event))
            return XR_OWN_AUDIT_IDENTITY_MISMATCH;
        if (!id_equal(owner->allocation_site_id, event->allocation_site_id))
            return XR_OWN_AUDIT_ORIGIN_MISMATCH;
        if (!domain_matches_manifest(event->domain, owner->initial_domain_contract_id,
                                     owner->initial_semantic_domain,
                                     owner->initial_materialization))
            return XR_OWN_AUDIT_DOMAIN_MISMATCH;
        if (audit->object_count == audit->object_capacity)
            return XR_OWN_AUDIT_CAPACITY_EXCEEDED;
        initialize_object(&candidate, owner, event);
        XrOwnershipAuditStatus status = check_generation_pin(audit, &candidate);
        if (status != XR_OWN_AUDIT_OK)
            return status;
    } else {
        if (!stored)
            return XR_OWN_AUDIT_UNKNOWN_OWNER;
        if (stored->terminal)
            return XR_OWN_AUDIT_INVALID_TRANSITION;
        candidate = *stored;
        XrOwnershipAuditStatus status = check_generation_pin(audit, &candidate);
        if (status != XR_OWN_AUDIT_OK)
            return status;
    }

    XrOwnershipAuditStatus status = validate_observation(transition, &candidate, event);
    if (status != XR_OWN_AUDIT_OK)
        return status;
    status = validate_event_fields(transition, &candidate, event);
    if (status != XR_OWN_AUDIT_OK)
        return status;
    status = update_logical_state(&candidate, transition);
    if (status != XR_OWN_AUDIT_OK)
        return status;
    status = update_physical_rc(&candidate, event);
    if (status != XR_OWN_AUDIT_OK)
        return status;

    XrAuditLoan *loan_slot = NULL;
    bool append_loan_slot = false;
    if (event->kind == XR_OWN_EVENT_BORROW) {
        if (id_is_zero(event->loan_id) ||
            find_active_loan(audit, candidate.key, event->loan_id))
            return XR_OWN_AUDIT_LOAN_MISMATCH;
        for (size_t i = 0; i < audit->loan_count; i++) {
            if (!audit->loans[i].active) {
                loan_slot = &audit->loans[i];
                break;
            }
        }
        if (!loan_slot) {
            if (audit->loan_count == audit->loan_capacity)
                return XR_OWN_AUDIT_CAPACITY_EXCEEDED;
            loan_slot = &audit->loans[audit->loan_count];
            append_loan_slot = true;
        }
        if (candidate.active_loans == UINT32_MAX)
            return XR_OWN_AUDIT_LOAN_MISMATCH;
        candidate.active_loans++;
    } else if (event->kind == XR_OWN_EVENT_END_BORROW) {
        loan_slot = find_active_loan(audit, candidate.key, event->loan_id);
        if (!loan_slot || candidate.active_loans == 0)
            return XR_OWN_AUDIT_LOAN_MISMATCH;
        candidate.active_loans--;
    }
    if (transition_has_domain_change(transition))
        candidate.domain = event->next_domain;
    if ((transition->flags & XR_OWN_AUDIT_TRANSITION_TERMINAL) != 0) {
        if (candidate.active_loans != 0)
            return XR_OWN_AUDIT_LOAN_MISMATCH;
        if (candidate.logical_balance != 0)
            return XR_OWN_AUDIT_INVALID_TRANSITION;
        if (event->kind == XR_OWN_EVENT_DESTROY && candidate.physical_seen &&
            candidate.last_physical_mode != XR_OWN_AUDIT_RC_STICKY &&
            candidate.last_physical_rc != 0)
            return XR_OWN_AUDIT_PHYSICAL_RC_MISMATCH;
        candidate.terminal = true;
    }

    if (event->kind == XR_OWN_EVENT_BORROW) {
        *loan_slot = (XrAuditLoan) {.loan_id = event->loan_id,
                                    .object = candidate.key,
                                    .active = true};
        if (append_loan_slot)
            audit->loan_count++;
    } else if (event->kind == XR_OWN_EVENT_END_BORROW) {
        loan_slot->active = false;
    }
    if (opens)
        audit->objects[audit->object_count++] = candidate;
    else
        *stored = candidate;
    return XR_OWN_AUDIT_OK;
}

XrOwnershipAuditStatus xr_ownership_audit_record(XrOwnershipAudit *audit,
                                                 const XrOwnershipAuditEvent *event) {
    if (!audit)
        return XR_OWN_AUDIT_INVALID_ARGUMENT;
    if (!enter_audit(audit))
        return status_load(audit);
    XrOwnershipAuditStatus result = status_load(audit);
    if (result != XR_OWN_AUDIT_OK)
        goto done;
    if (!event || event->kind >= XR_OWN_EVENT_COUNT ||
        id_is_zero(event->transition_id) || id_is_zero(event->operation_id) ||
        event->program_point >= XR_OWN_POINT_COUNT) {
        result = fail(audit, XR_OWN_AUDIT_INVALID_ARGUMENT, event);
        goto done;
    }
    if (audit->finished) {
        result = fail(audit, XR_OWN_AUDIT_ALREADY_FINISHED, event);
        goto done;
    }
    if (audit->event_count == audit->event_capacity) {
        result = fail(audit, XR_OWN_AUDIT_CAPACITY_EXCEEDED, event);
        goto done;
    }
    XrOwnershipAuditTransitionManifest *transition =
        find_transition(audit, event->transition_id);
    if (!transition) {
        result = fail(audit, XR_OWN_AUDIT_UNKNOWN_TRANSITION, event);
        goto done;
    }
    if (event->kind == XR_OWN_EVENT_PIN || event->kind == XR_OWN_EVENT_UNPIN)
        result = record_pin(audit, transition, event);
    else
        result = record_owner_event(audit, transition, event);
    if (result != XR_OWN_AUDIT_OK) {
        result = fail(audit, result, event);
        goto done;
    }
    audit->events[audit->event_count++] = *event;
    result = status_load(audit);
done:
    leave_audit(audit);
    return result;
}

XrOwnershipAuditStatus xr_ownership_audit_finish(XrOwnershipAudit *audit) {
    if (!audit)
        return XR_OWN_AUDIT_INVALID_ARGUMENT;
    if (!enter_audit(audit))
        return status_load(audit);
    XrOwnershipAuditStatus result = status_load(audit);
    if (result != XR_OWN_AUDIT_OK)
        goto done;
    if (audit->finished) {
        result = fail(audit, XR_OWN_AUDIT_ALREADY_FINISHED, NULL);
        goto done;
    }
    for (size_t i = 0; i < audit->object_count; i++) {
        if (!audit->objects[i].terminal || audit->objects[i].active_loans != 0) {
            result = fail(audit, XR_OWN_AUDIT_INCOMPLETE, NULL);
            goto done;
        }
    }
    for (size_t i = 0; i < audit->generation_count; i++) {
        if (audit->generations[i].pin_count != 0) {
            result = fail(audit, XR_OWN_AUDIT_GENERATION_PIN_MISMATCH, NULL);
            goto done;
        }
    }
    audit->finished = true;
    result = XR_OWN_AUDIT_OK;
done:
    leave_audit(audit);
    return result;
}

XrOwnershipAuditStatus xr_ownership_audit_status(const XrOwnershipAudit *audit) {
    return audit ? status_load(audit) : XR_OWN_AUDIT_INVALID_ARGUMENT;
}

const char *xr_ownership_audit_status_name(XrOwnershipAuditStatus status) {
    static const char *const names[] = {
        "ok", "invalid-argument", "out-of-memory", "capacity-exceeded", "reentrant",
        "already-finished", "duplicate-owner", "duplicate-transition", "duplicate-instance",
        "unknown-owner", "unknown-transition", "invalid-descriptor", "identity-mismatch",
        "origin-mismatch",
        "domain-mismatch", "invalid-transition", "loan-mismatch", "physical-rc-mismatch",
        "exit-mismatch", "destructor-mismatch", "generation-pin-missing",
        "generation-pin-mismatch", "incomplete",
    };
    return status >= XR_OWN_AUDIT_OK && status <= XR_OWN_AUDIT_INCOMPLETE
               ? names[status]
               : "unknown-status";
}

size_t xr_ownership_audit_event_count(const XrOwnershipAudit *audit) {
    return audit ? audit->event_count : 0;
}

const XrOwnershipAuditEvent *xr_ownership_audit_event(const XrOwnershipAudit *audit,
                                                      size_t index) {
    return audit && index < audit->event_count ? &audit->events[index] : NULL;
}

const XrOwnershipAuditEvent *xr_ownership_audit_failed_event(const XrOwnershipAudit *audit) {
    return audit && audit->has_failed_event ? &audit->failed_event : NULL;
}

const char *xr_ownership_audit_evidence_scope(void) {
    return "executed-path dynamic evidence, not formal proof";
}
