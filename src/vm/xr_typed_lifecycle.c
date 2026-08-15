/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_typed_lifecycle.c - TargetPlan-driven managed owner cleanup
 */

#include "xr_typed_lifecycle.h"
#include "../base/xmalloc.h"
#include "../plan/target/xr_target_profile.h"
#include "../runtime/abi/xr_runtime_target_authority.h"
#include <stdatomic.h>
#include <string.h>

struct XrTypedLifecycleOwnerContract {
    XrStableId identity;
    uint32_t slot;
    uint32_t state_operation;
    uint32_t normal_operation;
    uint32_t size;
    uint16_t alignment;
    uint16_t register_rep;
    uint16_t memory_rep;
    uint8_t terminal_cleanup_seen;
    uint8_t reserved;
};

typedef struct XrTypedLifecycleExecution {
    XrTypedLifecycleContext *context;
    uint32_t semantic_operation;
    XrTypedLifecycleExit exit_kind;
    XrTypedLifecycleStatus failure;
} XrTypedLifecycleExecution;

static XrTypedLifecycleOwnerContract *find_owner(
    const XrTypedLifecycleContext *context, uint32_t slot) {
    uint32_t low = 0;
    uint32_t high = context ? context->owner_count : 0;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        if (context->owners[middle].slot < slot)
            low = middle + 1u;
        else
            high = middle;
    }
    return context && low < context->owner_count &&
                   context->owners[low].slot == slot
               ? &context->owners[low]
               : NULL;
}

static bool representation_is_exact(
    const XrTargetPlan *plan, const XrTargetSlotRecord *slot,
    const XrRuntimeDynamicValueAbi *dynamic_value) {
    const XrTargetMachineRepRecord *register_rep =
        slot ? xr_target_plan_machine_rep(plan, slot->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        slot ? xr_target_plan_machine_rep(plan, slot->memory_rep) : NULL;
    return slot && register_rep && memory_rep &&
           register_rep->kind == XR_MACHINE_REP_DYN_VALUE &&
           memory_rep->kind == XR_MACHINE_REP_DYN_VALUE &&
           register_rep->root_kind == XR_TARGET_ROOT_DYNAMIC &&
           memory_rep->root_kind == XR_TARGET_ROOT_DYNAMIC &&
           register_rep->ownership == XR_TARGET_OWNERSHIP_OWNED &&
           memory_rep->ownership == XR_TARGET_OWNERSHIP_OWNED &&
           register_rep->memory_size == dynamic_value->size &&
           memory_rep->memory_size == dynamic_value->size &&
           register_rep->memory_align == dynamic_value->alignment &&
           memory_rep->memory_align == dynamic_value->alignment &&
           slot->root_kind == XR_TARGET_ROOT_DYNAMIC &&
           slot->ownership == XR_TARGET_OWNERSHIP_OWNED &&
           slot->size == dynamic_value->size &&
           slot->align == dynamic_value->alignment;
}

static bool build_native_profile(const XrTargetPlan *plan,
                                 XrRuntimeTargetAuthority *authority) {
    if (!plan || !authority ||
        xr_runtime_target_authority_native_hosted(authority) !=
            XR_RUNTIME_ABI_OK)
        return false;
    XrTargetProfileBuildInput input = {
        .machine = authority->machine,
        .runtime_abi = &authority->runtime_abi,
        .object_header_materialization =
            &authority->object_header_materialization,
        .string_contract = &authority->string_contract,
        .providers = authority->providers,
        .provider_count = authority->provider_count,
    };
    XrTargetProfile *native_profile = NULL;
    char error[256] = {0};
    bool built = xr_target_profile_build(&input, &native_profile, error,
                                         sizeof(error));
    bool exact = built && native_profile &&
                 xr_target_profile_require_exact(
                     xr_target_plan_profile(plan), native_profile, error,
                     sizeof(error));
    xr_target_profile_free(native_profile);
    return exact;
}

static bool collect_owner_contracts(
    XrTypedLifecycleContext *context,
    const XrRuntimeDynamicValueAbi *dynamic_value) {
    const XrTargetPlan *plan = context->plan;
    uint32_t function_count = 0;
    uint32_t slot_count = 0;
    uint32_t root_count = 0;
    uint32_t root_slot_count = 0;
    uint32_t cleanup_count = 0;
    uint32_t coroutine_count = 0;
    const XrTargetFunctionRecord *functions =
        xr_target_plan_functions(plan, &function_count);
    const XrTargetSlotRecord *slots = xr_target_plan_slots(plan, &slot_count);
    const XrTargetRootMapRecord *roots =
        xr_target_plan_root_maps(plan, &root_count);
    const uint32_t *root_slots =
        xr_target_plan_root_slots(plan, &root_slot_count);
    const XrTargetCleanupRecord *cleanups =
        xr_target_plan_cleanups(plan, &cleanup_count);
    const XrTargetCoroutineStateRecord *coroutines =
        xr_target_plan_coroutines(plan, &coroutine_count);
    const XrTargetFunctionRecord *function =
        functions && context->function < function_count
            ? &functions[context->function]
            : NULL;
    if (!function || function->id != context->function ||
        function->slot_begin > slot_count ||
        function->slot_count > slot_count - function->slot_begin ||
        function->root_begin > root_count ||
        function->root_count > root_count - function->root_begin ||
        function->cleanup_begin > cleanup_count ||
        function->cleanup_count > cleanup_count - function->cleanup_begin ||
        function->coroutine_begin > coroutine_count ||
        function->coroutine_count >
            coroutine_count - function->coroutine_begin ||
        function->root_count != 1u || function->coroutine_count != 1u ||
        function->cleanup_count != 2u ||
        (!slots && function->slot_count) ||
        (!roots && function->root_count) ||
        (!cleanups && function->cleanup_count) ||
        (!coroutines && function->coroutine_count))
        return false;

    const XrTargetRootMapRecord *root = &roots[function->root_begin];
    const XrTargetCoroutineStateRecord *state =
        &coroutines[function->coroutine_begin];
    if (root->id != function->root_begin ||
        root->function != function->id ||
        root->flags != (XR_TARGET_ROOT_SUSPEND |
                        XR_TARGET_ROOT_CANCEL | XR_TARGET_ROOT_EXIT) ||
        root->slot_begin >= root_slot_count || root->slot_count != 1u ||
        !root_slots || state->id != function->coroutine_begin ||
        state->function != function->id ||
        root->semantic_operation != state->semantic_operation)
        return false;

    uint32_t slot_index = root_slots[root->slot_begin];
    const XrTargetSlotRecord *slot =
        slot_index < slot_count ? &slots[slot_index] : NULL;
    if (!slot || slot->id != slot_index ||
        slot->function != function->id ||
        slot_index < function->slot_begin ||
        slot_index - function->slot_begin >= function->slot_count ||
        !representation_is_exact(plan, slot, dynamic_value))
        return false;
    context->owners =
        (XrTypedLifecycleOwnerContract *) xr_malloc(sizeof(*context->owners));
    if (!context->owners)
        return false;
    *context->owners = (XrTypedLifecycleOwnerContract) {
        .identity = slot->identity,
        .slot = slot_index,
        .state_operation = root->semantic_operation,
        .normal_operation = UINT32_MAX,
        .size = slot->size,
        .alignment = slot->align,
        .register_rep = slot->register_rep,
        .memory_rep = slot->memory_rep,
    };
    context->owner_count = 1u;

    uint32_t matched = 0;
    for (uint32_t i = 0; i < function->cleanup_count; i++) {
        const XrTargetCleanupRecord *cleanup =
            &cleanups[function->cleanup_begin + i];
        XrTypedLifecycleOwnerContract *owner =
            find_owner(context, cleanup->slot);
        if (!owner || cleanup->id != function->cleanup_begin + i ||
            cleanup->function != function->id ||
            cleanup->action != XR_TARGET_CLEANUP_RELEASE ||
            cleanup->provider != 0)
            return false;
        if (cleanup->flags ==
            (XR_TARGET_CLEANUP_CANCEL | XR_TARGET_CLEANUP_EXIT)) {
            if (cleanup->semantic_operation != owner->state_operation ||
                owner->terminal_cleanup_seen)
                return false;
            owner->terminal_cleanup_seen = 1u;
        } else if (cleanup->flags == 0) {
            if (cleanup->semantic_operation == owner->state_operation ||
                owner->normal_operation != UINT32_MAX)
                return false;
            owner->normal_operation = cleanup->semantic_operation;
        } else {
            return false;
        }
        matched++;
    }
    if (matched != function->cleanup_count)
        return false;
    return context->owners->normal_operation != UINT32_MAX &&
           context->owners->terminal_cleanup_seen;
}

static bool context_is_intact(const XrTypedLifecycleContext *context) {
    return context &&
           context->schema_version ==
               XR_TYPED_LIFECYCLE_CONTEXT_SCHEMA_VERSION &&
           context->plan && context->owners && context->owner_count &&
           context->resolve_object && context->reclaim_object &&
           xr_target_plan_is_verified(context->plan) &&
           xr_target_plan_schema_version(context->plan) ==
               XR_TYPED_FRAME_SUPPORTED_PLAN_SCHEMA_VERSION &&
           xr_target_plan_completed_family_mask(context->plan) ==
               XR_TYPED_FRAME_SUPPORTED_FAMILY_MASK &&
           xr_fingerprint_equal(context->plan_fingerprint,
                                xr_target_plan_fingerprint(context->plan)) &&
           xr_target_plan_fingerprint_is_intact(context->plan);
}

static bool request_matches_owner(
    const XrTypedLifecycleOwnerContract *owner, uint32_t semantic_operation,
    XrTypedLifecycleExit exit_kind) {
    if (!owner)
        return false;
    return exit_kind == XR_TYPED_LIFECYCLE_EXIT_NORMAL
               ? owner->normal_operation == semantic_operation
               : owner->state_operation == semantic_operation;
}

static uint8_t exit_event_flags(XrTypedLifecycleExit exit_kind) {
    if (exit_kind == XR_TYPED_LIFECYCLE_EXIT_CANCEL)
        return XR_TARGET_CLEANUP_CANCEL;
    if (exit_kind == XR_TYPED_LIFECYCLE_EXIT_ERROR ||
        exit_kind == XR_TYPED_LIFECYCLE_EXIT_RETURN)
        return XR_TARGET_CLEANUP_EXIT;
    return 0;
}

static bool carrier_padding_is_zero(const XrRuntimeDynamicValueAbi *abi,
                                    const uint8_t *bytes) {
    bool covered[UINT8_MAX + 1u] = {false};
    if (!abi || !bytes || abi->size > sizeof(covered))
        return false;
    for (uint32_t field = 0; field < XR_RUNTIME_DYNAMIC_FIELD_COUNT; field++) {
        const XrRuntimePhysicalFieldAbi *record = &abi->fields[field];
        if (record->offset > abi->size ||
            record->width > abi->size - record->offset)
            return false;
        for (uint32_t i = 0; i < record->width; i++)
            covered[record->offset + i] = true;
    }
    for (uint32_t i = 0; i < abi->size; i++)
        if (!covered[i] && bytes[i] != 0)
            return false;
    return true;
}

static bool read_native_uint(const uint8_t *bytes,
                             const XrRuntimePhysicalFieldAbi *field,
                             uint64_t *value) {
    if (!bytes || !field || !value || field->width == 0 ||
        field->width > sizeof(*value))
        return false;
    *value = 0;
    memcpy(value, bytes + field->offset, field->width);
    return true;
}

static const XrRuntimeDynamicTagAbiEntry *object_reference_tag(
    const XrRuntimeDynamicValueAbi *abi) {
    for (uint32_t i = 0; abi && i < abi->tag_count; i++)
        if (abi->tags[i].encoding == abi->object_reference_tag)
            return &abi->tags[i];
    return NULL;
}

static bool decode_string_header(XrTypedLifecycleContext *context,
                                 const XrTypedLifecycleOwnerContract *owner,
                                 const XrTypedSlotAccess *access,
                                 const uint8_t *bytes,
                                 XrRuntimeObjectHeader **header_out) {
    const XrRuntimeDynamicValueAbi *abi = &context->dynamic_value;
    if (header_out)
        *header_out = NULL;
    if (!owner || !access || !bytes || !header_out ||
        access->slot != owner->slot ||
        !xr_stable_id_equal(access->identity, owner->identity) ||
        access->size != owner->size ||
        access->alignment != owner->alignment ||
        access->register_rep != owner->register_rep ||
        access->memory_rep != owner->memory_rep ||
        access->size != abi->size || !carrier_padding_is_zero(abi, bytes))
        return false;
    uint64_t tag = 0;
    uint64_t flags = 0;
    uint64_t payload = 0;
    if (!read_native_uint(bytes, &abi->fields[0], &tag) ||
        !read_native_uint(bytes, &abi->fields[1], &flags) ||
        !read_native_uint(bytes, &abi->fields[2], &payload) ||
        tag != abi->object_reference_tag || payload == 0 ||
        payload > UINTPTR_MAX)
        return false;
    const XrRuntimeDynamicTagAbiEntry *tag_contract =
        object_reference_tag(abi);
    if (!tag_contract ||
        tag_contract->payload_kind !=
            XR_RUNTIME_DYN_PAYLOAD_OBJECT_REFERENCE ||
        (flags & tag_contract->required_flags) !=
            tag_contract->required_flags ||
        (flags & ~tag_contract->allowed_flags) != 0)
        return false;
    XrRuntimeObjectHeader *header = context->resolve_object(
        context->allocation_context, (uintptr_t) payload);
    const XrString *string = (const XrString *) header;
    if (!header || (uintptr_t) header != (uintptr_t) payload ||
        xr_runtime_string_object_validate_prefix(string) !=
            XR_RUNTIME_ABI_OK ||
        header->domain_id != XR_RUNTIME_STRING_DOMAIN_EXEC_LOCAL ||
        atomic_load_explicit(&string->traits, memory_order_acquire) !=
            XR_RUNTIME_STRING_TRAIT_LOCAL)
        return false;
    *header_out = header;
    return true;
}

static XrTypedFrameStatus execute_release(
    void *opaque, uint8_t action, const XrTypedSlotAccess *access,
    void *bytes) {
    XrTypedLifecycleExecution *execution =
        (XrTypedLifecycleExecution *) opaque;
    XrTypedLifecycleContext *context =
        execution ? execution->context : NULL;
    XrTypedLifecycleOwnerContract *owner =
        context && access ? find_owner(context, access->slot) : NULL;
    if (!execution || !context || action != XR_TARGET_CLEANUP_RELEASE ||
        !request_matches_owner(owner, execution->semantic_operation,
                               execution->exit_kind)) {
        if (execution)
            execution->failure = XR_TYPED_LIFECYCLE_CONTRACT_UNAVAILABLE;
        return XR_TYPED_FRAME_ACCESS_MISMATCH;
    }
    XrRuntimeObjectHeader *header = NULL;
    if (!decode_string_header(context, owner, access,
                              (const uint8_t *) bytes, &header)) {
        execution->failure = XR_TYPED_LIFECYCLE_CARRIER_INVALID;
        return XR_TYPED_FRAME_ACCESS_MISMATCH;
    }
    int32_t before = atomic_load_explicit(&header->rc, memory_order_acquire);
    if (before <= 0) {
        execution->failure = XR_TYPED_LIFECYCLE_CARRIER_INVALID;
        return XR_TYPED_FRAME_ACCESS_MISMATCH;
    }
    bool last = false;
    if (xr_runtime_object_header_release(header, &last) !=
        XR_RUNTIME_ABI_OK) {
        execution->failure = XR_TYPED_LIFECYCLE_RELEASE_FAILED;
        return XR_TYPED_FRAME_ACCESS_MISMATCH;
    }
    int32_t after = last
                        ? 0
                        : atomic_load_explicit(&header->rc,
                                               memory_order_acquire);
    if (last)
        context->reclaim_object(context->allocation_context, header);
    if (context->observer) {
        XrTypedLifecycleEvent event = {
            .plan_fingerprint = context->plan_fingerprint,
            .slot_identity = owner->identity,
            .function = context->function,
            .semantic_operation = execution->semantic_operation,
            .slot = owner->slot,
            .physical_rc_before = before,
            .physical_rc_after = after,
            .action = action,
            .exit_kind = (uint8_t) execution->exit_kind,
            .physical_last_release = last ? 1u : 0u,
        };
        if (context->observer(context->observer_context, &event) !=
            XR_TYPED_LIFECYCLE_OK)
            execution->failure = XR_TYPED_LIFECYCLE_AUDIT_REJECTED;
    }
    return XR_TYPED_FRAME_OK;
}

XR_FUNC XrTypedLifecycleStatus xr_typed_lifecycle_context_init(
    const XrTargetPlan *verified_plan,
    const XrFingerprint *required_plan_fingerprint, uint32_t function,
    const XrTypedLifecycleBindings *bindings,
    XrTypedLifecycleContext *context) {
    if (context)
        memset(context, 0, sizeof(*context));
    if (!verified_plan || !required_plan_fingerprint || !bindings ||
        !bindings->resolve_object || !bindings->reclaim_object || !context)
        return XR_TYPED_LIFECYCLE_INVALID_ARGUMENT;
    if (!xr_target_plan_is_verified(verified_plan) ||
        xr_target_plan_schema_version(verified_plan) !=
            XR_TYPED_FRAME_SUPPORTED_PLAN_SCHEMA_VERSION ||
        xr_target_plan_completed_family_mask(verified_plan) !=
            XR_TYPED_FRAME_SUPPORTED_FAMILY_MASK ||
        !xr_target_plan_fingerprint_is_intact(verified_plan))
        return XR_TYPED_LIFECYCLE_PLAN_NOT_VERIFIED;
    if (!xr_fingerprint_equal(xr_target_plan_fingerprint(verified_plan),
                              *required_plan_fingerprint))
        return XR_TYPED_LIFECYCLE_PLAN_IDENTITY_MISMATCH;

    XrRuntimeTargetAuthority authority;
    if (!build_native_profile(verified_plan, &authority))
        return XR_TYPED_LIFECYCLE_TARGET_PROFILE_MISMATCH;
    context->schema_version = XR_TYPED_LIFECYCLE_CONTEXT_SCHEMA_VERSION;
    context->function = function;
    context->plan = xr_target_plan_retain((XrTargetPlan *) verified_plan);
    context->plan_fingerprint = *required_plan_fingerprint;
    context->dynamic_value = authority.runtime_abi.dynamic_value;
    context->resolve_object = bindings->resolve_object;
    context->reclaim_object = bindings->reclaim_object;
    context->allocation_context = bindings->allocation_context;
    context->observer = bindings->observer;
    context->observer_context = bindings->observer_context;
    if (!collect_owner_contracts(context, &context->dynamic_value)) {
        xr_typed_lifecycle_context_dispose(context);
        return XR_TYPED_LIFECYCLE_CONTRACT_UNAVAILABLE;
    }
    return XR_TYPED_LIFECYCLE_OK;
}

XR_FUNC void xr_typed_lifecycle_context_dispose(
    XrTypedLifecycleContext *context) {
    if (!context)
        return;
    xr_free(context->owners);
    xr_target_plan_free(context->plan);
    memset(context, 0, sizeof(*context));
}

XR_FUNC XrTypedLifecycleStatus xr_typed_lifecycle_execute(
    XrTypedLifecycleContext *context, XrTypedFrame *frame,
    uint32_t semantic_operation, XrTypedLifecycleExit exit_kind,
    uint32_t *executed) {
    if (executed)
        *executed = 0;
    if (!context || !frame || !executed ||
        exit_kind >= XR_TYPED_LIFECYCLE_EXIT_COUNT)
        return XR_TYPED_LIFECYCLE_INVALID_ARGUMENT;
    if (!context_is_intact(context))
        return XR_TYPED_LIFECYCLE_PLAN_IDENTITY_MISMATCH;
    XrTypedFrameContext frame_context;
    if (xr_typed_frame_context(frame, &frame_context) != XR_TYPED_FRAME_OK)
        return XR_TYPED_LIFECYCLE_FRAME_ERROR;
    if (!xr_fingerprint_equal(frame_context.function_identity.plan_fingerprint,
                              context->plan_fingerprint) ||
        frame_context.function_identity.function != context->function)
        return XR_TYPED_LIFECYCLE_PLAN_IDENTITY_MISMATCH;
    bool matched = false;
    for (uint32_t i = 0; i < context->owner_count; i++)
        matched |= request_matches_owner(&context->owners[i],
                                         semantic_operation, exit_kind);
    if (!matched)
        return XR_TYPED_LIFECYCLE_CONTRACT_UNAVAILABLE;

    XrTypedLifecycleExecution execution = {
        .context = context,
        .semantic_operation = semantic_operation,
        .exit_kind = exit_kind,
        .failure = XR_TYPED_LIFECYCLE_OK,
    };
    XrTypedFrameStatus frame_status = xr_typed_frame_execute_cleanups(
        frame, semantic_operation, exit_event_flags(exit_kind),
        execute_release, &execution, executed);
    if (execution.failure != XR_TYPED_LIFECYCLE_OK)
        return execution.failure;
    if (frame_status == XR_TYPED_FRAME_OK)
        return XR_TYPED_LIFECYCLE_OK;
    if (frame_status == XR_TYPED_FRAME_LIFECYCLE_INACTIVE ||
        frame_status == XR_TYPED_FRAME_TERMINAL)
        return XR_TYPED_LIFECYCLE_ALREADY_EXECUTED;
    if (frame_status == XR_TYPED_FRAME_PLAN_IDENTITY_MISMATCH)
        return XR_TYPED_LIFECYCLE_PLAN_IDENTITY_MISMATCH;
    return XR_TYPED_LIFECYCLE_FRAME_ERROR;
}
