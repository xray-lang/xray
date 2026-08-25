/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_typed_lifecycle_audit.c - Managed cleanup certificate adapter
 */

#include "vm/audit/xr_typed_lifecycle_audit.h"
#include "base/xmalloc.h"
#include "plan/ownership/xr_ownership_certificate.h"
#include "plan/semantic/xr_semantic_plan.h"
#include "plan/target/xr_target_plan.h"
#include "plan/target/xr_target_profile.h"
#include "runtime/abi/xr_runtime_target_authority.h"
#include "runtime/abi/xr_runtime_target_profile.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

typedef struct XrTypedLifecycleCertificateBinding {
    XrStableId owner_id;
    XrStableId transition_id;
    XrStableId operation_id;
    XrStableId exit_id;
    XrStableId allocation_site_id;
    XrStableId frame_id;
    XrFingerprint premise_fingerprint;
    uint32_t owner_index;
    uint32_t event_index;
    uint32_t release_operation;
    uint32_t event_block;
    uint32_t event_successor;
    int16_t logical_delta;
    uint8_t owner_initial_state;
    uint8_t owner_return_provenance;
    uint8_t owner_flags;
    uint8_t state_after;
    uint8_t kind;
    uint8_t program_point;
    uint8_t event_reserved;
    char owner_key[256];
    char event_key[224];
} XrTypedLifecycleCertificateBinding;

struct XrTypedLifecycleAuditContext {
    uint32_t schema_version;
    uint32_t function;
    uint32_t semantic_function;
    uint32_t slot;
    uint32_t producer_operation;
    uint32_t producer_value;
    uint32_t normal_operation;
    uint32_t state_operation;
    XrSemanticPlan *semantic_plan;
    XrTargetPlan *target_plan;
    const XrOwnershipCertificate *certificate;
    XrOwnershipAudit *oracle;
    XrFingerprint target_fingerprint;
    XrFingerprint semantic_fingerprint;
    XrStableId invocation_id;
    uint64_t next_activation_epoch;
    XrTargetFunctionRecord target_function;
    XrTargetSlotRecord target_slot;
    XrTargetRootMapRecord target_root;
    XrTargetCleanupRecord target_cleanups[2];
    XrTargetCoroutineStateRecord target_state;
    XrTypedLifecycleCertificateBinding certificate_binding;
    XrRuntimeStringObjectContract string_contract;
    atomic_int status;
    atomic_int oracle_status;
    atomic_flag gate;
    bool record_physical_rc;
};

static bool id_is_zero(XrStableId id) {
    static const XrStableId zero = {{0}};
    return xr_stable_id_equal(id, zero);
}

static bool fingerprint_is_zero(XrFingerprint fingerprint) {
    static const XrFingerprint zero = {{0}};
    return xr_fingerprint_equal(fingerprint, zero);
}

static void set_status(XrTypedLifecycleAuditContext *context, XrTypedLifecycleAuditStatus status,
                       XrOwnershipAuditStatus oracle_status) {
    int expected = XR_TYPED_LIFECYCLE_AUDIT_OK;
    if (!context || status == XR_TYPED_LIFECYCLE_AUDIT_OK)
        return;
    if (atomic_compare_exchange_strong_explicit(&context->status, &expected, status,
                                                memory_order_acq_rel, memory_order_acquire))
        atomic_store_explicit(&context->oracle_status, oracle_status, memory_order_release);
}

static bool exact_target_partition(XrTypedLifecycleAuditContext *context) {
    uint32_t function_count = 0;
    uint32_t slot_count = 0;
    uint32_t root_count = 0;
    uint32_t root_slot_count = 0;
    uint32_t cleanup_count = 0;
    uint32_t coroutine_count = 0;
    const XrTargetFunctionRecord *functions =
        xr_target_plan_functions(context->target_plan, &function_count);
    const XrTargetSlotRecord *slots = xr_target_plan_slots(context->target_plan, &slot_count);
    const XrTargetRootMapRecord *roots =
        xr_target_plan_root_maps(context->target_plan, &root_count);
    const uint32_t *root_slots = xr_target_plan_root_slots(context->target_plan, &root_slot_count);
    const XrTargetCleanupRecord *cleanups =
        xr_target_plan_cleanups(context->target_plan, &cleanup_count);
    const XrTargetCoroutineStateRecord *states =
        xr_target_plan_coroutines(context->target_plan, &coroutine_count);
    const XrTargetFunctionRecord *function =
        functions && context->function < function_count ? &functions[context->function] : NULL;
    if (!function || function->id != context->function || function->root_count != 1u ||
        function->cleanup_count != 2u || function->coroutine_count != 1u ||
        function->root_begin >= root_count || function->cleanup_begin > cleanup_count ||
        function->cleanup_count > cleanup_count - function->cleanup_begin ||
        function->coroutine_begin >= coroutine_count || !slots || !roots || !root_slots ||
        !cleanups || !states)
        return false;

    const XrTargetRootMapRecord *root = &roots[function->root_begin];
    const XrTargetCoroutineStateRecord *state = &states[function->coroutine_begin];
    if (root->id != function->root_begin || root->function != function->id ||
        root->slot_count != 1u || root->slot_begin >= root_slot_count ||
        root->flags != (XR_TARGET_ROOT_SUSPEND | XR_TARGET_ROOT_CANCEL | XR_TARGET_ROOT_EXIT) ||
        state->id != function->coroutine_begin || state->function != function->id ||
        state->semantic_operation != root->semantic_operation)
        return false;
    uint32_t slot_index = root_slots[root->slot_begin];
    const XrTargetSlotRecord *slot = slot_index < slot_count ? &slots[slot_index] : NULL;
    if (!slot || slot->id != slot_index || slot->function != function->id ||
        slot->root_kind != XR_TARGET_ROOT_DYNAMIC || slot->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        slot->semantic_value == XR_SEMANTIC_INDEX_NONE ||
        slot->semantic_operation == XR_SEMANTIC_INDEX_NONE)
        return false;

    uint32_t normal = XR_SEMANTIC_INDEX_NONE;
    uint32_t terminal = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < function->cleanup_count; i++) {
        const XrTargetCleanupRecord *cleanup = &cleanups[function->cleanup_begin + i];
        if (cleanup->id != function->cleanup_begin + i || cleanup->function != function->id ||
            cleanup->slot != slot_index || cleanup->action != XR_TARGET_CLEANUP_RELEASE ||
            cleanup->provider != 0)
            return false;
        if (cleanup->flags == 0 && normal == XR_SEMANTIC_INDEX_NONE)
            normal = cleanup->semantic_operation;
        else if (cleanup->flags == (XR_TARGET_CLEANUP_CANCEL | XR_TARGET_CLEANUP_EXIT) &&
                 terminal == XR_SEMANTIC_INDEX_NONE)
            terminal = cleanup->semantic_operation;
        else
            return false;
    }
    if (normal == XR_SEMANTIC_INDEX_NONE || terminal != root->semantic_operation ||
        normal == terminal)
        return false;

    context->semantic_function = function->semantic_function;
    context->slot = slot_index;
    context->producer_operation = slot->semantic_operation;
    context->producer_value = slot->semantic_value;
    context->normal_operation = normal;
    context->state_operation = terminal;
    context->target_function = *function;
    context->target_slot = *slot;
    context->target_root = *root;
    context->target_state = *state;
    memcpy(context->target_cleanups, cleanups + function->cleanup_begin,
           sizeof(context->target_cleanups));
    return true;
}

static bool build_owner_key(const XrSemanticOperationRecord *producer, uint32_t value, char *key,
                            size_t key_size, XrStableId *identity) {
    char operation_id[XR_STABLE_ID_BYTES * 2u + 1u];
    xr_stable_id_hex(producer->id, operation_id);
    int written = snprintf(key, key_size, "owner-v2:%s:value=%u", operation_id, value);
    XrFingerprint digest = {{0}};
    return written >= 0 && (size_t) written < key_size &&
           xr_stable_id_from_key(key, identity, &digest);
}

static bool build_event_key(const XrOwnershipOwnerRecord *owner,
                            const XrSemanticOperationRecord *operation,
                            const XrOwnershipEventRecord *event, uint32_t ordinal, char *key,
                            size_t key_size, XrStableId *identity) {
    char owner_id[XR_STABLE_ID_BYTES * 2u + 1u];
    char operation_id[XR_STABLE_ID_BYTES * 2u + 1u];
    xr_stable_id_hex(owner->id, owner_id);
    xr_stable_id_hex(operation->id, operation_id);
    int written = snprintf(key, key_size, "ownership-event-v4:%s:%s:%u:%u:%u:%u:%u", owner_id,
                           operation_id, event->block, event->successor, (unsigned) event->kind,
                           (unsigned) event->program_point, ordinal);
    XrFingerprint digest = {{0}};
    return written >= 0 && (size_t) written < key_size &&
           xr_stable_id_from_key(key, identity, &digest);
}

static bool event_exit_identity(const XrSemanticPlan *semantic, const XrOwnershipEventRecord *event,
                                const XrSemanticOperationRecord *operation, XrStableId *identity) {
    if (!semantic || !event || !operation || !identity ||
        event->block >= xr_semantic_plan_block_count(semantic))
        return false;
    const XrSemanticBlockRecord *block = xr_semantic_plan_block(semantic, event->block);
    if (!block || block->function != operation->function)
        return false;
    if (event->program_point == XR_OWN_POINT_AFTER_OPERATION &&
        event->successor == XR_SEMANTIC_INDEX_NONE) {
        *identity = operation->id;
        return true;
    }
    if (event->program_point == XR_OWN_POINT_BLOCK_EXIT &&
        event->successor == XR_SEMANTIC_INDEX_NONE) {
        *identity = block->id;
        return true;
    }
    if (event->program_point != XR_OWN_POINT_EDGE || event->successor == XR_SEMANTIC_INDEX_NONE)
        return false;
    const XrSemanticEdgeRecord *match = NULL;
    size_t edge_count = xr_semantic_plan_edge_count(semantic);
    for (size_t i = 0; i < edge_count; i++) {
        const XrSemanticEdgeRecord *candidate = xr_semantic_plan_edge(semantic, (uint32_t) i);
        if (!candidate || candidate->function != operation->function ||
            candidate->from_block != event->block || candidate->to_block != event->successor)
            continue;
        if (match)
            return false;
        match = candidate;
    }
    if (!match)
        return false;
    *identity = match->id;
    return true;
}

static bool bind_certificate_records(XrTypedLifecycleAuditContext *context,
                                     const XrOwnershipCertificate *certificate,
                                     const XrSemanticFunctionRecord *function,
                                     const XrSemanticOperationRecord *producer,
                                     const XrSemanticOperationRecord *release,
                                     const XrOwnershipOwnerRecord *owner, uint32_t owner_index,
                                     const XrOwnershipEventRecord *event, uint32_t event_index) {
    XrTypedLifecycleCertificateBinding *binding = &context->certificate_binding;
    binding->owner_id = owner->id;
    binding->transition_id = event->id;
    binding->operation_id = release->id;
    binding->allocation_site_id = producer->allocation_id;
    binding->frame_id = function->id;
    binding->premise_fingerprint = xr_ownership_certificate_fingerprint(certificate);
    binding->owner_index = owner_index;
    binding->event_index = event_index;
    binding->release_operation = event->operation;
    binding->event_block = event->block;
    binding->event_successor = event->successor;
    binding->logical_delta = event->logical_delta;
    binding->owner_initial_state = owner->initial_state;
    binding->owner_return_provenance = owner->return_provenance;
    binding->owner_flags = owner->flags;
    binding->state_after = event->state_after;
    binding->kind = event->kind;
    binding->program_point = event->program_point;
    binding->event_reserved = event->reserved;
    context->certificate = certificate;
    return !fingerprint_is_zero(binding->premise_fingerprint);
}

static bool exact_certificate_binding(XrTypedLifecycleAuditContext *context) {
    const XrSemanticPlan *semantic = context->semantic_plan;
    const XrOwnershipCertificate *certificate = xr_semantic_plan_ownership(semantic);
    const XrSemanticFunctionRecord *function =
        xr_semantic_plan_function(semantic, context->semantic_function);
    const XrSemanticOperationRecord *producer =
        xr_semantic_plan_operation(semantic, context->producer_operation);
    const XrSemanticOperationRecord *release =
        xr_semantic_plan_operation(semantic, context->normal_operation);
    if (!certificate || !function || !producer || !release ||
        producer->function != context->semantic_function ||
        producer->result_value != context->producer_value ||
        release->function != context->semantic_function || id_is_zero(producer->allocation_id))
        return false;

    XrTypedLifecycleCertificateBinding *binding = &context->certificate_binding;
    XrStableId expected_owner = {{0}};
    if (!build_owner_key(producer, context->producer_value, binding->owner_key,
                         sizeof(binding->owner_key), &expected_owner))
        return false;
    const XrOwnershipOwnerRecord *owner_match = NULL;
    uint32_t owner_match_index = 0;
    size_t owner_count = xr_ownership_certificate_owner_count(certificate);
    for (size_t i = 0; i < owner_count; i++) {
        const XrOwnershipOwnerRecord *owner =
            xr_ownership_certificate_owner(certificate, (uint32_t) i);
        if (!owner || owner->function != context->semantic_function ||
            owner->origin_value != context->producer_value)
            continue;
        if (owner_match)
            return false;
        owner_match = owner;
        owner_match_index = (uint32_t) i;
    }
    if (!owner_match || !owner_match->canonical_key ||
        strcmp(owner_match->canonical_key, binding->owner_key) != 0 ||
        !xr_stable_id_equal(owner_match->id, expected_owner) ||
        owner_match->exit_state != XR_OWN_RELEASED ||
        owner_match->initial_state >= XR_OWN_STATE_COUNT)
        return false;

    const XrOwnershipEventRecord *event_match = NULL;
    uint32_t event_match_index = 0;
    size_t event_count = xr_ownership_certificate_event_count(certificate);
    for (size_t i = 0; i < event_count; i++) {
        const XrOwnershipEventRecord *event =
            xr_ownership_certificate_event(certificate, (uint32_t) i);
        if (!event || event->owner != owner_match_index ||
            event->operation != context->normal_operation || event->kind != XR_OWN_EVENT_RELEASE ||
            event->logical_delta != -1 || event->state_after != XR_OWN_RELEASED)
            continue;
        if (event_match)
            return false;
        event_match = event;
        event_match_index = (uint32_t) i;
    }
    XrStableId expected_event = {{0}};
    if (!event_match || !event_match->canonical_key ||
        !build_event_key(owner_match, release, event_match, event_match_index, binding->event_key,
                         sizeof(binding->event_key), &expected_event) ||
        strcmp(event_match->canonical_key, binding->event_key) != 0 ||
        !xr_stable_id_equal(event_match->id, expected_event) ||
        !event_exit_identity(semantic, event_match, release, &binding->exit_id))
        return false;

    return bind_certificate_records(context, certificate, function, producer, release, owner_match,
                                    owner_match_index, event_match, event_match_index);
}

static bool load_exact_string_authority(XrTypedLifecycleAuditContext *context) {
    XrRuntimeTargetAuthority authority;
    if (xr_runtime_target_authority_native_hosted(&authority) != XR_RUNTIME_ABI_OK)
        return false;
    XrTargetProfile *native_profile = NULL;
    char error[256] = {0};
    bool exact = xr_runtime_target_profile_build_native_hosted(
                     &native_profile, error, sizeof(error)) &&
                 native_profile &&
                 xr_target_profile_require_exact(xr_target_plan_profile(context->target_plan),
                                                 native_profile, error, sizeof(error));
    xr_target_profile_free(native_profile);
    if (!exact)
        return false;
    context->string_contract = authority.string_contract;
    return true;
}

static bool register_oracle_contract(XrTypedLifecycleAuditContext *context) {
    const XrTypedLifecycleCertificateBinding *binding = &context->certificate_binding;
    const XrRuntimeDomainIdentity domain =
        context->string_contract.domains[XR_RUNTIME_STRING_DOMAIN_EXEC_LOCAL];
    XrOwnershipAuditOwnerManifest owner = {
        .owner_id = binding->owner_id,
        .descriptor = &context->string_contract.layout,
        .extent = &context->string_contract.extent,
        .allocation_site_id = binding->allocation_site_id,
        .frame_id = binding->frame_id,
        .destructor_id = context->string_contract.layout.destructor_id,
        .premise_fingerprint = binding->premise_fingerprint,
        .initial_domain_contract_id = domain.contract_id,
        .initial_logical_balance = 1,
        .initial_state = binding->owner_initial_state,
        .initial_semantic_domain = domain.semantic_domain,
        .initial_materialization = domain.materialization,
    };
    XrOwnershipAuditStatus status = xr_ownership_audit_register_owner(context->oracle, &owner);
    if (status != XR_OWN_AUDIT_OK) {
        atomic_store_explicit(&context->oracle_status, status, memory_order_release);
        return false;
    }
    XrOwnershipAuditTransitionManifest transition = {
        .transition_id = binding->transition_id,
        .owner_id = binding->owner_id,
        .operation_id = binding->operation_id,
        .exit_id = binding->exit_id,
        .state_before_mask = XR_OWN_STATE_MASK(binding->owner_initial_state),
        .flags = XR_OWN_AUDIT_TRANSITION_OPENS_INSTANCE | XR_OWN_AUDIT_TRANSITION_TERMINAL,
        .logical_delta = binding->logical_delta,
        .kind = binding->kind,
        .state_after = binding->state_after,
        .program_point = binding->program_point,
        .physical_rc_mode =
            context->record_physical_rc ? XR_OWN_AUDIT_RC_LOCAL : XR_OWN_AUDIT_RC_NONE,
    };
    status = xr_ownership_audit_register_transition(context->oracle, &transition);
    atomic_store_explicit(&context->oracle_status, status, memory_order_release);
    return status == XR_OWN_AUDIT_OK;
}

static bool certificate_records_are_intact(const XrTypedLifecycleAuditContext *context) {
    const XrTypedLifecycleCertificateBinding *binding = &context->certificate_binding;
    const XrOwnershipOwnerRecord *owner =
        xr_ownership_certificate_owner(context->certificate, binding->owner_index);
    const XrOwnershipEventRecord *event =
        xr_ownership_certificate_event(context->certificate, binding->event_index);
    const XrSemanticOperationRecord *producer =
        xr_semantic_plan_operation(context->semantic_plan, context->producer_operation);
    const XrSemanticOperationRecord *release =
        xr_semantic_plan_operation(context->semantic_plan, context->normal_operation);
    char owner_key[sizeof(binding->owner_key)] = {0};
    char event_key[sizeof(binding->event_key)] = {0};
    XrStableId owner_id = {{0}};
    XrStableId event_id = {{0}};
    XrStableId exit_id = {{0}};
    return owner && event && owner->canonical_key && event->canonical_key && producer && release &&
           build_owner_key(producer, context->producer_value, owner_key, sizeof(owner_key),
                           &owner_id) &&
           build_event_key(owner, release, event, binding->event_index, event_key,
                           sizeof(event_key), &event_id) &&
           event_exit_identity(context->semantic_plan, event, release, &exit_id) &&
           strcmp(owner_key, binding->owner_key) == 0 &&
           strcmp(event_key, binding->event_key) == 0 &&
           xr_stable_id_equal(owner_id, binding->owner_id) &&
           xr_stable_id_equal(event_id, binding->transition_id) &&
           xr_stable_id_equal(exit_id, binding->exit_id) &&
           strcmp(owner->canonical_key, binding->owner_key) == 0 &&
           strcmp(event->canonical_key, binding->event_key) == 0 &&
           xr_stable_id_equal(owner->id, binding->owner_id) &&
           owner->function == context->semantic_function &&
           owner->origin_value == context->producer_value &&
           owner->initial_state == binding->owner_initial_state &&
           owner->exit_state == XR_OWN_RELEASED &&
           owner->return_provenance == binding->owner_return_provenance &&
           owner->flags == binding->owner_flags &&
           xr_stable_id_equal(event->id, binding->transition_id) &&
           event->owner == binding->owner_index && event->operation == binding->release_operation &&
           event->block == binding->event_block && event->successor == binding->event_successor &&
           event->logical_delta == binding->logical_delta && event->kind == binding->kind &&
           event->state_after == binding->state_after &&
           event->program_point == binding->program_point &&
           event->reserved == binding->event_reserved &&
           xr_fingerprint_equal(xr_ownership_certificate_fingerprint(context->certificate),
                                binding->premise_fingerprint);
}

static bool target_records_are_intact(const XrTypedLifecycleAuditContext *context) {
    uint32_t function_count = 0;
    uint32_t slot_count = 0;
    uint32_t root_count = 0;
    uint32_t root_slot_count = 0;
    uint32_t cleanup_count = 0;
    uint32_t state_count = 0;
    const XrTargetFunctionRecord *functions =
        xr_target_plan_functions(context->target_plan, &function_count);
    const XrTargetSlotRecord *slots = xr_target_plan_slots(context->target_plan, &slot_count);
    const XrTargetRootMapRecord *roots =
        xr_target_plan_root_maps(context->target_plan, &root_count);
    const uint32_t *root_slots = xr_target_plan_root_slots(context->target_plan, &root_slot_count);
    const XrTargetCleanupRecord *cleanups =
        xr_target_plan_cleanups(context->target_plan, &cleanup_count);
    const XrTargetCoroutineStateRecord *states =
        xr_target_plan_coroutines(context->target_plan, &state_count);
    const XrTargetFunctionRecord *function = &context->target_function;
    return functions && slots && roots && root_slots && cleanups && states &&
           context->function < function_count && context->slot < slot_count &&
           function->root_begin < root_count && cleanup_count >= 2u &&
           function->cleanup_begin <= cleanup_count - 2u &&
           function->coroutine_begin < state_count &&
           context->target_root.slot_begin < root_slot_count &&
           memcmp(&functions[context->function], function, sizeof(*function)) == 0 &&
           memcmp(&slots[context->slot], &context->target_slot, sizeof(context->target_slot)) ==
               0 &&
           memcmp(&roots[function->root_begin], &context->target_root,
                  sizeof(context->target_root)) == 0 &&
           root_slots[context->target_root.slot_begin] == context->slot &&
           memcmp(cleanups + function->cleanup_begin, context->target_cleanups,
                  sizeof(context->target_cleanups)) == 0 &&
           memcmp(&states[function->coroutine_begin], &context->target_state,
                  sizeof(context->target_state)) == 0;
}

static bool authority_is_intact(const XrTypedLifecycleAuditContext *context) {
    if (!context || context->schema_version != XR_TYPED_LIFECYCLE_AUDIT_SCHEMA_VERSION ||
        !context->semantic_plan || !context->target_plan || !context->oracle ||
        !xr_semantic_plan_is_verified(context->semantic_plan) ||
        !xr_target_plan_is_verified(context->target_plan) ||
        !xr_target_plan_fingerprint_is_intact(context->target_plan) ||
        !xr_fingerprint_equal(xr_target_plan_fingerprint(context->target_plan),
                              context->target_fingerprint) ||
        !xr_fingerprint_equal(xr_semantic_plan_fingerprint(context->semantic_plan),
                              context->semantic_fingerprint) ||
        !xr_fingerprint_equal(xr_target_plan_semantic_fingerprint(context->target_plan),
                              context->semantic_fingerprint) ||
        xr_semantic_plan_ownership(context->semantic_plan) != context->certificate ||
        !target_records_are_intact(context) || !certificate_records_are_intact(context))
        return false;
    const XrSemanticFunctionRecord *function =
        xr_semantic_plan_function(context->semantic_plan, context->semantic_function);
    const XrSemanticOperationRecord *producer =
        xr_semantic_plan_operation(context->semantic_plan, context->producer_operation);
    const XrSemanticOperationRecord *release =
        xr_semantic_plan_operation(context->semantic_plan, context->normal_operation);
    return function && producer && release &&
           xr_stable_id_equal(function->id, context->certificate_binding.frame_id) &&
           xr_stable_id_equal(producer->allocation_id,
                              context->certificate_binding.allocation_site_id) &&
           producer->function == context->semantic_function &&
           producer->result_value == context->producer_value &&
           release->function == context->semantic_function &&
           xr_stable_id_equal(release->id, context->certificate_binding.operation_id);
}

static bool observation_is_exact(const XrTypedLifecycleAuditContext *context,
                                 const XrTypedLifecycleEvent *event) {
    if (!event || event->action != XR_TARGET_CLEANUP_RELEASE || event->reserved != 0 ||
        event->function != context->function || event->slot != context->slot ||
        !xr_stable_id_equal(event->slot_identity, context->target_slot.identity) ||
        !xr_fingerprint_equal(event->plan_fingerprint, context->target_fingerprint) ||
        event->exit_kind >= XR_TYPED_LIFECYCLE_EXIT_COUNT || event->physical_last_release > 1u ||
        event->physical_rc_before < 1)
        return false;
    uint32_t operation = event->exit_kind == XR_TYPED_LIFECYCLE_EXIT_NORMAL
                             ? context->normal_operation
                             : context->state_operation;
    if (event->semantic_operation != operation)
        return false;
    if (event->physical_last_release)
        return event->physical_rc_before == 1 && event->physical_rc_after == 0;
    return event->physical_rc_before > 1 &&
           event->physical_rc_after == event->physical_rc_before - 1;
}

static XrOwnershipAuditEvent make_oracle_event(const XrTypedLifecycleAuditContext *context,
                                               const XrTypedLifecycleEvent *observation,
                                               uint64_t activation_epoch) {
    const XrTypedLifecycleCertificateBinding *binding = &context->certificate_binding;
    XrOwnershipAuditEvent event = {
        .object =
            {
                .owner_id = binding->owner_id,
                .invocation_id = context->invocation_id,
                .activation_epoch = activation_epoch,
            },
        .transition_id = binding->transition_id,
        .layout_id = context->string_contract.layout.layout_id,
        .allocation_site_id = binding->allocation_site_id,
        .operation_id = binding->operation_id,
        .exit_id = binding->exit_id,
        .frame_id = binding->frame_id,
        .premise_fingerprint = binding->premise_fingerprint,
        .domain = context->string_contract.domains[XR_RUNTIME_STRING_DOMAIN_EXEC_LOCAL],
        .kind = binding->kind,
        .program_point = binding->program_point,
    };
    if (context->record_physical_rc) {
        event.physical_rc_before = observation->physical_rc_before;
        event.physical_rc_after = observation->physical_rc_after;
        event.flags = XR_OWN_AUDIT_EVENT_PHYSICAL_RC;
        event.physical_rc_mode = XR_OWN_AUDIT_RC_LOCAL;
    }
    return event;
}

XrTypedLifecycleAuditContext *
xr_typed_lifecycle_audit_create(const XrTypedLifecycleAuditConfig *config,
                                XrTypedLifecycleAuditStatus *status) {
    if (status)
        *status = XR_TYPED_LIFECYCLE_AUDIT_INVALID_ARGUMENT;
    if (!config || !config->verified_semantic_plan || !config->verified_target_plan ||
        !config->required_target_plan_fingerprint || !config->oracle ||
        id_is_zero(config->invocation_id) || config->first_activation_epoch == 0)
        return NULL;
    if (!xr_semantic_plan_is_verified(config->verified_semantic_plan) ||
        !xr_target_plan_is_verified(config->verified_target_plan) ||
        !xr_target_plan_fingerprint_is_intact(config->verified_target_plan) ||
        !xr_fingerprint_equal(xr_target_plan_fingerprint(config->verified_target_plan),
                              *config->required_target_plan_fingerprint) ||
        !xr_fingerprint_equal(xr_target_plan_semantic_fingerprint(config->verified_target_plan),
                              xr_semantic_plan_fingerprint(config->verified_semantic_plan))) {
        if (status)
            *status = XR_TYPED_LIFECYCLE_AUDIT_PLAN_MISMATCH;
        return NULL;
    }
    XrTypedLifecycleAuditContext *context =
        (XrTypedLifecycleAuditContext *) xr_calloc(1, sizeof(*context));
    if (!context) {
        if (status)
            *status = XR_TYPED_LIFECYCLE_AUDIT_OUT_OF_MEMORY;
        return NULL;
    }
    context->schema_version = XR_TYPED_LIFECYCLE_AUDIT_SCHEMA_VERSION;
    context->function = config->function;
    context->semantic_plan =
        xr_semantic_plan_retain((XrSemanticPlan *) config->verified_semantic_plan);
    context->target_plan = xr_target_plan_retain((XrTargetPlan *) config->verified_target_plan);
    context->oracle = config->oracle;
    context->target_fingerprint = *config->required_target_plan_fingerprint;
    context->semantic_fingerprint = xr_semantic_plan_fingerprint(config->verified_semantic_plan);
    context->invocation_id = config->invocation_id;
    context->next_activation_epoch = config->first_activation_epoch;
    context->record_physical_rc = config->record_physical_rc;
    atomic_init(&context->status, XR_TYPED_LIFECYCLE_AUDIT_OK);
    atomic_init(&context->oracle_status, XR_OWN_AUDIT_OK);
    atomic_flag_clear(&context->gate);
    if (!exact_target_partition(context)) {
        if (status)
            *status = XR_TYPED_LIFECYCLE_AUDIT_PLAN_MISMATCH;
        xr_typed_lifecycle_audit_destroy(context);
        return NULL;
    }
    if (!load_exact_string_authority(context)) {
        if (status)
            *status = XR_TYPED_LIFECYCLE_AUDIT_PLAN_MISMATCH;
        xr_typed_lifecycle_audit_destroy(context);
        return NULL;
    }
    if (!exact_certificate_binding(context)) {
        if (status)
            *status = XR_TYPED_LIFECYCLE_AUDIT_CERTIFICATE_MISMATCH;
        xr_typed_lifecycle_audit_destroy(context);
        return NULL;
    }
    if (!register_oracle_contract(context)) {
        if (status)
            *status = XR_TYPED_LIFECYCLE_AUDIT_ORACLE_REJECTED;
        xr_typed_lifecycle_audit_destroy(context);
        return NULL;
    }
    if (status)
        *status = XR_TYPED_LIFECYCLE_AUDIT_OK;
    return context;
}

void xr_typed_lifecycle_audit_destroy(XrTypedLifecycleAuditContext *context) {
    if (!context)
        return;
    xr_semantic_plan_free(context->semantic_plan);
    xr_target_plan_free(context->target_plan);
    memset(context, 0, sizeof(*context));
    xr_free(context);
}

XrTypedLifecycleStatus xr_typed_lifecycle_audit_observe(void *opaque,
                                                        const XrTypedLifecycleEvent *event) {
    XrTypedLifecycleAuditContext *context = (XrTypedLifecycleAuditContext *) opaque;
    if (!context)
        return XR_TYPED_LIFECYCLE_AUDIT_REJECTED;
    if (atomic_flag_test_and_set_explicit(&context->gate, memory_order_acquire)) {
        set_status(context, XR_TYPED_LIFECYCLE_AUDIT_REENTRANT, XR_OWN_AUDIT_REENTRANT);
        return XR_TYPED_LIFECYCLE_AUDIT_REJECTED;
    }
    XrTypedLifecycleStatus result = XR_TYPED_LIFECYCLE_AUDIT_REJECTED;
    if (atomic_load_explicit(&context->status, memory_order_acquire) != XR_TYPED_LIFECYCLE_AUDIT_OK)
        goto done;
    if (!authority_is_intact(context) || !observation_is_exact(context, event)) {
        set_status(context, XR_TYPED_LIFECYCLE_AUDIT_PLAN_MISMATCH, XR_OWN_AUDIT_IDENTITY_MISMATCH);
        goto done;
    }
    if (context->next_activation_epoch == UINT64_MAX) {
        set_status(context, XR_TYPED_LIFECYCLE_AUDIT_ACTIVATION_EXHAUSTED,
                   XR_OWN_AUDIT_CAPACITY_EXCEEDED);
        goto done;
    }
    XrOwnershipAuditEvent oracle_event =
        make_oracle_event(context, event, context->next_activation_epoch);
    XrOwnershipAuditStatus oracle_status =
        xr_ownership_audit_record(context->oracle, &oracle_event);
    if (oracle_status != XR_OWN_AUDIT_OK) {
        set_status(context, XR_TYPED_LIFECYCLE_AUDIT_ORACLE_REJECTED, oracle_status);
        goto done;
    }
    context->next_activation_epoch++;
    result = XR_TYPED_LIFECYCLE_OK;
done:
    atomic_flag_clear_explicit(&context->gate, memory_order_release);
    return result;
}

XrTypedLifecycleAuditStatus
xr_typed_lifecycle_audit_status(const XrTypedLifecycleAuditContext *context) {
    return context ? (XrTypedLifecycleAuditStatus) atomic_load_explicit(&context->status,
                                                                        memory_order_acquire)
                   : XR_TYPED_LIFECYCLE_AUDIT_INVALID_ARGUMENT;
}

XrOwnershipAuditStatus
xr_typed_lifecycle_audit_oracle_status(const XrTypedLifecycleAuditContext *context) {
    return context ? (XrOwnershipAuditStatus) atomic_load_explicit(&context->oracle_status,
                                                                   memory_order_acquire)
                   : XR_OWN_AUDIT_INVALID_ARGUMENT;
}

const char *xr_typed_lifecycle_audit_status_name(XrTypedLifecycleAuditStatus status) {
    switch (status) {
        case XR_TYPED_LIFECYCLE_AUDIT_OK:
            return "ok";
        case XR_TYPED_LIFECYCLE_AUDIT_INVALID_ARGUMENT:
            return "invalid-argument";
        case XR_TYPED_LIFECYCLE_AUDIT_PLAN_MISMATCH:
            return "plan-mismatch";
        case XR_TYPED_LIFECYCLE_AUDIT_CERTIFICATE_MISMATCH:
            return "certificate-mismatch";
        case XR_TYPED_LIFECYCLE_AUDIT_ORACLE_REJECTED:
            return "oracle-rejected";
        case XR_TYPED_LIFECYCLE_AUDIT_REENTRANT:
            return "reentrant";
        case XR_TYPED_LIFECYCLE_AUDIT_ACTIVATION_EXHAUSTED:
            return "activation-exhausted";
        case XR_TYPED_LIFECYCLE_AUDIT_OUT_OF_MEMORY:
            return "out-of-memory";
    }
    return "unknown";
}
