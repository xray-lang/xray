/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_execution.c - Exact program/profile/provider execution binding
 */

#include "xr_execution.h"

#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../program/xr_validated_program_internal.h"

#include <stdatomic.h>
#include <string.h>

typedef struct XrBoundOperation {
    XrStableId operation_id;
    XrProviderTrampolineKind trampoline_kind;
    union {
        XrProviderI64UnaryEntry i64_unary;
        XrProviderI64NullaryEntry i64_nullary;
        XrProviderBoolI64UnaryEntry bool_i64_unary;
        XrProviderOptionalI64PairNullaryEntry optional_i64_pair_nullary;
        XrProviderOutputWriteEntry output_write;
        XrProviderTypedEntry typed;
    } entry;
    void *context;
} XrBoundOperation;

typedef struct XrBoundProvider {
    XrStableId contract_id;
    XrFingerprint contract_fingerprint;
    uint32_t behavior_flags;
    XrBoundOperation *operations;
    uint16_t operation_count;
} XrBoundProvider;

typedef struct XrExecutionLeaseTicket {
    uint64_t id;
    uint32_t in_flight_calls;
    bool resource_pin;
} XrExecutionLeaseTicket;

typedef struct XrExecutionRuntimeState {
    const void *layout_key;
    void *value;
    void (*destroy)(void *);
} XrExecutionRuntimeState;

struct XrExecutionResource {
    XrExecutionResourcePin pin;
    XrStableId resource_id;
    void *payload;
    void (*destroy)(void *);
};

struct XrInstance {
    XrValidatedProgram *program;
    XrTargetProfile *profile;
    XrExecutionId execution_id;
    uint64_t generation;
    XrBoundProvider *providers;
    size_t provider_count;
    atomic_uint_least32_t state;
    atomic_uint_least64_t leases;
    atomic_bool lease_lock;
    XrExecutionLeaseTicket *lease_tickets;
    size_t lease_ticket_capacity;
    uint64_t next_lease_ticket;
    uint64_t resource_pins;
    uint64_t initialization_ticket;
    uint32_t initialized_modules;
    bool initializer_active;
    bool initialization_failed;
    XrExecutionRuntimeState runtime_state;
    bool state_cleanup_active;
};

static void lease_lock(XrInstance *instance) {
    bool expected = false;
    while (!atomic_compare_exchange_weak_explicit(&instance->lease_lock, &expected, true,
                                                  memory_order_acquire,
                                                  memory_order_relaxed)) {
        expected = false;
    }
}

static void lease_unlock(XrInstance *instance) {
    atomic_store_explicit(&instance->lease_lock, false, memory_order_release);
}

static XrExecutionRuntimeState take_drained_state_locked(XrInstance *instance) {
    XrExecutionRuntimeState state = {0};
    if (atomic_load_explicit(&instance->state, memory_order_relaxed) == XR_INSTANCE_DRAINING &&
        atomic_load_explicit(&instance->leases, memory_order_relaxed) == 0u &&
        instance->runtime_state.value) {
        XR_CHECK(!instance->state_cleanup_active, "instance state cleanup already active");
        state = instance->runtime_state;
        instance->runtime_state = (XrExecutionRuntimeState) {0};
        instance->state_cleanup_active = true;
    }
    return state;
}

static void destroy_drained_state(XrInstance *instance, XrExecutionRuntimeState state) {
    if (!state.value)
        return;
    state.destroy(state.value);
    lease_lock(instance);
    XR_CHECK(instance->state_cleanup_active, "instance state cleanup lost its retirement pin");
    instance->state_cleanup_active = false;
    lease_unlock(instance);
}

static XrExecutionLeaseTicket *find_ticket_locked(XrInstance *instance, uint64_t ticket,
                                                   bool resource_pin) {
    if (ticket == 0u)
        return NULL;
    for (size_t index = 0; index < instance->lease_ticket_capacity; ++index) {
        if (instance->lease_tickets[index].id == ticket &&
            instance->lease_tickets[index].resource_pin == resource_pin)
            return &instance->lease_tickets[index];
    }
    return NULL;
}

static XrExecutionLeaseTicket *find_lease_ticket_locked(XrInstance *instance, uint64_t ticket) {
    return find_ticket_locked(instance, ticket, false);
}

static bool lease_ticket_is_active_locked(XrInstance *instance, uint64_t ticket) {
    return find_lease_ticket_locked(instance, ticket) != NULL;
}

static void clear_diagnostic(XrExecutionDiagnostic *diagnostic) {
    if (diagnostic)
        memset(diagnostic, 0, sizeof(*diagnostic));
}

static XrExecutionStatus reject(XrExecutionDiagnostic *diagnostic, XrExecutionDiagnosticKind kind,
                                uint32_t provider, uint32_t operation, XrStableId contract_id,
                                XrStableId operation_id, XrExecutionStatus status) {
    if (diagnostic) {
        diagnostic->kind = kind;
        diagnostic->provider_index = provider;
        diagnostic->operation_index = operation;
        diagnostic->contract_id = contract_id;
        diagnostic->operation_id = operation_id;
    }
    return status;
}

static bool stable_id_equal(XrStableId left, XrStableId right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

static const XrTargetProviderOperationContract *find_profile_operation(
    const XrTargetProviderContract *contract, XrStableId operation_id) {
    for (uint16_t index = 0; contract && index < contract->operation_count; ++index) {
        if (stable_id_equal(contract->operations[index].stable_id, operation_id))
            return &contract->operations[index];
    }
    return NULL;
}

static const XrTargetProviderContract *find_profile_provider(const XrTargetProfile *profile,
                                                             XrStableId contract_id) {
    size_t count = xr_target_profile_provider_count(profile);
    for (size_t index = 0; index < count; ++index) {
        const XrTargetProviderContract *contract = xr_target_profile_provider(profile, index);
        if (contract && stable_id_equal(contract->contract_id, contract_id))
            return contract;
    }
    return NULL;
}

static uint32_t required_provider_behavior(const XrProgramProviderRequirementView *requirement) {
    uint32_t required = 0u;
    for (uint32_t index = 0; requirement && index < requirement->operation_count; ++index) {
        const XrProviderLogicalContract *logical = &requirement->operations[index].logical_contract;
        if (logical->threads == XR_PROVIDER_THREADS_ANY)
            required |= XR_PROVIDER_BEHAVIOR_THREAD_SAFE;
        if (logical->reentry == XR_PROVIDER_REENTRY_ALLOWED)
            required |= XR_PROVIDER_BEHAVIOR_REENTRANT;
        if (logical->callbacks == XR_PROVIDER_CALLBACK_SYNCHRONOUS)
            required |= XR_PROVIDER_BEHAVIOR_CALLBACK_SAFE;
    }
    return required;
}

static bool provider_slot_is_scalar_i64(const XrTargetProviderCallSlotAbi *slot) {
    return slot && slot->value_kind == XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER &&
           slot->width == 8u && slot->alignment == 8u &&
           slot->ownership == XR_TARGET_PROVIDER_CALL_OWNERSHIP_NONE && slot->flags == 0u;
}

static bool provider_operation_uses_i64_unary_trampoline(
    const XrTargetProviderOperationContract *operation) {
    const XrTargetProviderCallAbiContract *abi = operation ? &operation->call_abi : NULL;
    return abi && abi->schema_version == XR_RUNTIME_ABI_SCHEMA_VERSION &&
           abi->calling_convention == XR_TARGET_PROVIDER_CALLING_CONVENTION_C &&
           abi->variadic == 0u && abi->parameter_count == 1u &&
           provider_slot_is_scalar_i64(&abi->parameters[0]) &&
           provider_slot_is_scalar_i64(&abi->result) && operation->lifetime_flags == 0u &&
           operation->failure_flags == 0u;
}

static bool provider_slot_is_consumed_i64(const XrTargetProviderCallSlotAbi *slot) {
    return slot && slot->value_kind == XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER &&
           slot->width == 8u && slot->alignment == 8u &&
           slot->ownership == XR_TARGET_PROVIDER_CALL_OWNERSHIP_CONSUMED && slot->flags == 0u;
}

static bool provider_operation_uses_i64_nullary_trampoline(
    const XrTargetProviderOperationContract *operation) {
    const XrTargetProviderCallAbiContract *abi = operation ? &operation->call_abi : NULL;
    return abi && abi->schema_version == XR_RUNTIME_ABI_SCHEMA_VERSION &&
           abi->calling_convention == XR_TARGET_PROVIDER_CALLING_CONVENTION_C &&
           abi->variadic == 0u && abi->parameter_count == 0u &&
           provider_slot_is_scalar_i64(&abi->result) && operation->lifetime_flags == 0u &&
           operation->failure_flags == 0u;
}

static bool provider_operation_uses_bool_i64_unary_trampoline(
    const XrTargetProviderOperationContract *operation) {
    const XrTargetProviderCallAbiContract *abi = operation ? &operation->call_abi : NULL;
    return abi && abi->schema_version == XR_RUNTIME_ABI_SCHEMA_VERSION &&
           abi->calling_convention == XR_TARGET_PROVIDER_CALLING_CONVENTION_C &&
           abi->variadic == 0u && abi->parameter_count == 1u &&
           provider_slot_is_consumed_i64(&abi->parameters[0]) &&
           abi->result.value_kind == XR_TARGET_PROVIDER_CALL_VALUE_UNSIGNED_INTEGER &&
           abi->result.width == 1u && abi->result.alignment == 1u &&
           abi->result.ownership == XR_TARGET_PROVIDER_CALL_OWNERSHIP_NONE &&
           abi->result.flags == 0u && operation->effect_flags == XR_TARGET_PROVIDER_EFFECT_IO &&
           operation->lifetime_flags == XR_TARGET_PROVIDER_LIFETIME_CONSUMES_OWNED &&
           operation->failure_flags == 0u;
}

static bool provider_slot_is_output_pointer(const XrTargetProviderCallSlotAbi *slot,
                                            uint8_t pointer_width, uint8_t pointer_alignment,
                                            uint8_t flags);

static bool provider_operation_uses_optional_i64_pair_nullary_trampoline(
    const XrTargetProviderOperationContract *operation) {
    const XrTargetProviderCallAbiContract *abi = operation ? &operation->call_abi : NULL;
    return abi && abi->schema_version == XR_RUNTIME_ABI_SCHEMA_VERSION &&
           abi->calling_convention == XR_TARGET_PROVIDER_CALLING_CONVENTION_C &&
           abi->variadic == 0u && abi->parameter_count == 3u &&
           abi->result.value_kind == XR_TARGET_PROVIDER_CALL_VALUE_UNSIGNED_INTEGER &&
           abi->result.width == 1u && abi->result.alignment == 1u &&
           abi->result.ownership == XR_TARGET_PROVIDER_CALL_OWNERSHIP_NONE &&
           abi->result.flags == 0u &&
           provider_slot_is_output_pointer(&abi->parameters[0], abi->pointer_width,
                                           abi->pointer_alignment, 0u) &&
           provider_slot_is_output_pointer(&abi->parameters[1], abi->pointer_width,
                                           abi->pointer_alignment, 0u) &&
           provider_slot_is_output_pointer(&abi->parameters[2], abi->pointer_width,
                                           abi->pointer_alignment, 0u) &&
           operation->effect_flags == XR_TARGET_PROVIDER_EFFECT_IO &&
           operation->lifetime_flags == XR_TARGET_PROVIDER_LIFETIME_BORROWS &&
           operation->failure_flags == XR_TARGET_PROVIDER_FAILURE_RETURNS_STATUS;
}

static XrProviderTrampolineKind program_operation_trampoline_kind(const XrValidatedProgram *program,
                                                                  uint32_t requirement_index,
                                                                  uint32_t operation_index) {
    if (!program)
        return XR_PROVIDER_TRAMPOLINE_INVALID;
    XrProviderTrampolineKind found = XR_PROVIDER_TRAMPOLINE_INVALID;
    for (uint32_t function = 0; function < program->function_count; ++function) {
        const XrValidatedFunction *function_row = &program->functions[function];
        for (uint32_t block = 0; block < function_row->block_count; ++block) {
            const XrValidatedBlock *block_row = &function_row->blocks[block];
            for (uint32_t instruction = 0; instruction < block_row->instruction_count;
                 ++instruction) {
                const XrValidatedInstruction *op = &block_row->instructions[instruction];
                if ((op->operation_id != XR_CORE_OP_CORE_PROVIDER_CALL &&
                     op->operation_id != XR_CORE_OP_CORE_OUTPUT_GROUP) ||
                    op->immediate.provider_operation.requirement_index != requirement_index ||
                    op->immediate.provider_operation.operation_index != operation_index)
                    continue;
                XrProviderTrampolineKind candidate = XR_PROVIDER_TRAMPOLINE_OUTPUT_WRITE;
                if (op->operation_id == XR_CORE_OP_CORE_PROVIDER_CALL) {
                    uint32_t provider_operand_count = op->operand_count;
                    if (op->successor_count == 1u) {
                        const XrValidatedBlock *target =
                            &function_row->blocks[op->successors[0]];
                        provider_operand_count -= target->argument_count;
                    }
                    uint16_t operand_type = provider_operand_count == 1u
                                                ? function_row->value_types[op->operands[0]]
                                                : XR_CORE_TYPE_VOID;
                    switch (xr_validated_program_provider_call_kind(
                        program, op->result_type_id,
                        provider_operand_count == 1u ? &operand_type : NULL,
                        provider_operand_count)) {
                        case XR_PROVIDER_LOGICAL_CALL_I64_UNARY:
                            candidate = XR_PROVIDER_TRAMPOLINE_I64_UNARY;
                            break;
                        case XR_PROVIDER_LOGICAL_CALL_I64_NULLARY:
                            candidate = XR_PROVIDER_TRAMPOLINE_I64_NULLARY;
                            break;
                        case XR_PROVIDER_LOGICAL_CALL_BOOL_I64_UNARY:
                            candidate = XR_PROVIDER_TRAMPOLINE_BOOL_I64_UNARY;
                            break;
                        case XR_PROVIDER_LOGICAL_CALL_OPTIONAL_I64_PAIR_NULLARY:
                            candidate = XR_PROVIDER_TRAMPOLINE_OPTIONAL_I64_PAIR_NULLARY;
                            break;
                        default:
                            candidate = XR_PROVIDER_TRAMPOLINE_TYPED;
                            break;
                    }
                }
                if (found != XR_PROVIDER_TRAMPOLINE_INVALID && found != candidate)
                    return XR_PROVIDER_TRAMPOLINE_INVALID;
                found = candidate;
            }
        }
    }
    return found;
}

static bool provider_slot_is_output_pointer(const XrTargetProviderCallSlotAbi *slot,
                                            uint8_t pointer_width, uint8_t pointer_alignment,
                                            uint8_t flags) {
    return slot && slot->value_kind == XR_TARGET_PROVIDER_CALL_VALUE_DATA_ADDRESS &&
           slot->width == pointer_width && slot->alignment == pointer_alignment &&
           slot->ownership == XR_TARGET_PROVIDER_CALL_OWNERSHIP_BORROWED &&
           slot->flags == flags;
}

static bool provider_operation_uses_output_write_trampoline(
    const XrTargetProviderOperationContract *operation) {
    const XrTargetProviderCallAbiContract *abi = operation ? &operation->call_abi : NULL;
    return abi && abi->schema_version == XR_RUNTIME_ABI_SCHEMA_VERSION &&
           abi->calling_convention == XR_TARGET_PROVIDER_CALLING_CONVENTION_C &&
           abi->variadic == 0u && abi->parameter_count == 3u &&
           abi->result.value_kind == XR_TARGET_PROVIDER_CALL_VALUE_UNSIGNED_INTEGER &&
           abi->result.width == 1u && abi->result.alignment == 1u &&
           abi->result.ownership == XR_TARGET_PROVIDER_CALL_OWNERSHIP_NONE &&
           abi->result.flags == 0u &&
           provider_slot_is_output_pointer(&abi->parameters[0], abi->pointer_width,
                                           abi->pointer_alignment,
                                           XR_TARGET_PROVIDER_CALL_SLOT_NULLABLE) &&
           provider_slot_is_output_pointer(&abi->parameters[1], abi->pointer_width,
                                           abi->pointer_alignment,
                                           XR_TARGET_PROVIDER_CALL_SLOT_CONST_POINTEE) &&
           abi->parameters[2].value_kind == XR_TARGET_PROVIDER_CALL_VALUE_UNSIGNED_INTEGER &&
           abi->parameters[2].width == abi->pointer_width &&
           abi->parameters[2].alignment == abi->pointer_alignment &&
           abi->parameters[2].ownership == XR_TARGET_PROVIDER_CALL_OWNERSHIP_NONE &&
           abi->parameters[2].flags == 0u &&
           operation->effect_flags == XR_TARGET_PROVIDER_EFFECT_IO &&
           operation->lifetime_flags == XR_TARGET_PROVIDER_LIFETIME_BORROWS &&
           operation->failure_flags == XR_TARGET_PROVIDER_FAILURE_RETURNS_STATUS;
}

static bool provider_operation_uses_typed_trampoline(const XrTargetProviderOperationContract *operation) {
    const XrTargetProviderCallAbiContract *abi = operation ? &operation->call_abi : NULL;
    return abi && abi->schema_version == XR_RUNTIME_ABI_SCHEMA_VERSION &&
           abi->calling_convention == XR_TARGET_PROVIDER_CALLING_CONVENTION_C &&
           abi->variadic == 0u && abi->parameter_count == 3u &&
           abi->result.value_kind == XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER &&
           abi->result.width == 4u && abi->result.alignment == 4u &&
           abi->result.ownership == XR_TARGET_PROVIDER_CALL_OWNERSHIP_NONE && abi->result.flags == 0u &&
           provider_slot_is_output_pointer(&abi->parameters[0], abi->pointer_width, abi->pointer_alignment,
                                            XR_TARGET_PROVIDER_CALL_SLOT_NULLABLE) &&
           provider_slot_is_output_pointer(&abi->parameters[1], abi->pointer_width, abi->pointer_alignment,
                                            XR_TARGET_PROVIDER_CALL_SLOT_CONST_POINTEE) &&
           provider_slot_is_output_pointer(&abi->parameters[2], abi->pointer_width, abi->pointer_alignment, 0u) &&
           operation->failure_flags == XR_TARGET_PROVIDER_FAILURE_RETURNS_STATUS &&
           operation->lifetime_flags == XR_TARGET_PROVIDER_LIFETIME_BORROWS;
}

static XrExecutionStatus validate_bindings(const XrExecutionBindingInput *input,
                                           XrExecutionDiagnostic *diagnostic) {
    uint32_t expected_count = xr_validated_program_provider_requirement_count(input->program);
    if (input->provider_count != expected_count ||
        (expected_count == 0u ? input->providers != NULL : input->providers == NULL))
        return reject(diagnostic, XR_EXECUTION_DIAGNOSTIC_PROVIDER_COUNT, 0, 0, (XrStableId) {{0}},
                      (XrStableId) {{0}}, XR_EXECUTION_PROVIDER_REJECTED);
    for (uint32_t provider = 0; provider < expected_count; ++provider) {
        XrProgramProviderRequirementView requirement = {0};
        if (!xr_validated_program_provider_requirement(input->program, provider, &requirement))
            return reject(diagnostic, XR_EXECUTION_DIAGNOSTIC_INVALID_INPUT, provider, 0,
                          (XrStableId) {{0}}, (XrStableId) {{0}},
                          XR_EXECUTION_INVALID_INPUT);
        const XrTargetProviderContract *expected =
            find_profile_provider(input->profile, requirement.contract_id);
        if (!expected)
            return reject(diagnostic, XR_EXECUTION_DIAGNOSTIC_PROFILE, provider, 0,
                          requirement.contract_id, (XrStableId) {{0}},
                          XR_EXECUTION_PROFILE_REJECTED);
        const XrProviderBinding *actual = &input->providers[provider];
        XrFingerprint expected_fingerprint = {{0}};
        if (xr_target_provider_contract_fingerprint(expected, &expected_fingerprint) !=
                XR_RUNTIME_ABI_OK ||
            !stable_id_equal(requirement.contract_id, actual->contract_id) ||
            !xr_fingerprint_equal(expected_fingerprint, actual->contract_fingerprint))
            return reject(diagnostic, XR_EXECUTION_DIAGNOSTIC_PROVIDER_CONTRACT,
                          provider, 0, requirement.contract_id, (XrStableId) {{0}},
                          XR_EXECUTION_PROVIDER_REJECTED);
        uint32_t required_behavior = required_provider_behavior(&requirement);
        if (actual->reserved16 != 0 ||
            (actual->behavior_flags & ~XR_PROVIDER_BEHAVIOR_FLAGS_ALL) != 0u ||
            (actual->behavior_flags & required_behavior) != required_behavior)
            return reject(diagnostic, XR_EXECUTION_DIAGNOSTIC_PROVIDER_BEHAVIOR,
                          provider, 0, expected->contract_id, (XrStableId) {{0}},
                          XR_EXECUTION_PROVIDER_REJECTED);
        if (actual->operation_count != requirement.operation_count || !actual->operations)
            return reject(diagnostic, XR_EXECUTION_DIAGNOSTIC_PROVIDER_OPERATION,
                          provider, 0, expected->contract_id, (XrStableId) {{0}},
                          XR_EXECUTION_PROVIDER_REJECTED);
        for (uint16_t operation = 0; operation < actual->operation_count; ++operation) {
            const XrTargetProviderOperationContract *expected_operation =
                find_profile_operation(expected, requirement.operations[operation].operation_id);
            const XrProviderOperationBinding *actual_operation = &actual->operations[operation];
            if (!expected_operation || !xr_provider_logical_contract_equal(
                                           &requirement.operations[operation].logical_contract,
                                           &expected_operation->logical_contract))
                return reject(diagnostic, XR_EXECUTION_DIAGNOSTIC_PROFILE, provider, operation,
                              expected->contract_id, requirement.operations[operation].operation_id,
                              XR_EXECUTION_PROFILE_REJECTED);
            XrProviderTrampolineKind expected_trampoline = XR_PROVIDER_TRAMPOLINE_INVALID;
            if (provider_operation_uses_i64_unary_trampoline(expected_operation))
                expected_trampoline = XR_PROVIDER_TRAMPOLINE_I64_UNARY;
            else if (provider_operation_uses_i64_nullary_trampoline(expected_operation))
                expected_trampoline = XR_PROVIDER_TRAMPOLINE_I64_NULLARY;
            else if (provider_operation_uses_bool_i64_unary_trampoline(expected_operation))
                expected_trampoline = XR_PROVIDER_TRAMPOLINE_BOOL_I64_UNARY;
            else if (provider_operation_uses_optional_i64_pair_nullary_trampoline(
                         expected_operation))
                expected_trampoline = XR_PROVIDER_TRAMPOLINE_OPTIONAL_I64_PAIR_NULLARY;
            else if (provider_operation_uses_output_write_trampoline(expected_operation))
                expected_trampoline = XR_PROVIDER_TRAMPOLINE_OUTPUT_WRITE;
            else if (provider_operation_uses_typed_trampoline(expected_operation))
                expected_trampoline = XR_PROVIDER_TRAMPOLINE_TYPED;
            XrProviderTrampolineKind program_trampoline =
                program_operation_trampoline_kind(input->program, provider, operation);
            if (expected_trampoline == XR_PROVIDER_TRAMPOLINE_INVALID ||
                (expected_trampoline == XR_PROVIDER_TRAMPOLINE_TYPED
                     ? program_trampoline == XR_PROVIDER_TRAMPOLINE_INVALID ||
                       program_trampoline == XR_PROVIDER_TRAMPOLINE_OUTPUT_WRITE
                     : expected_trampoline != program_trampoline))
                return reject(diagnostic, XR_EXECUTION_DIAGNOSTIC_PROVIDER_ABI, provider,
                              operation, expected->contract_id, expected_operation->stable_id,
                              XR_EXECUTION_PROVIDER_REJECTED);
            if (!stable_id_equal(requirement.operations[operation].operation_id,
                                 actual_operation->operation_id) ||
                actual_operation->trampoline_kind != expected_trampoline ||
                (expected_trampoline == XR_PROVIDER_TRAMPOLINE_I64_UNARY
                     ? actual_operation->entry.i64_unary == NULL
                 : expected_trampoline == XR_PROVIDER_TRAMPOLINE_I64_NULLARY
                     ? actual_operation->entry.i64_nullary == NULL
                 : expected_trampoline == XR_PROVIDER_TRAMPOLINE_BOOL_I64_UNARY
                     ? actual_operation->entry.bool_i64_unary == NULL
                 : expected_trampoline == XR_PROVIDER_TRAMPOLINE_OPTIONAL_I64_PAIR_NULLARY
                     ? actual_operation->entry.optional_i64_pair_nullary == NULL
                 : expected_trampoline == XR_PROVIDER_TRAMPOLINE_TYPED
                     ? actual_operation->entry.typed == NULL
                     : actual_operation->entry.output_write == NULL))
                return reject(diagnostic, XR_EXECUTION_DIAGNOSTIC_PROVIDER_OPERATION,
                              provider, operation, expected->contract_id,
                              expected_operation->stable_id, XR_EXECUTION_PROVIDER_REJECTED);
        }
    }
    return XR_EXECUTION_OK;
}

static void destroy_instance(XrInstance *instance) {
    if (!instance)
        return;
    for (size_t provider = 0; provider < instance->provider_count; ++provider)
        xr_free(instance->providers[provider].operations);
    xr_free(instance->lease_tickets);
    xr_free(instance->providers);
    xr_target_profile_free(instance->profile);
    xr_validated_program_free(instance->program);
    xr_free(instance);
}

static bool copy_bindings(XrInstance *instance, const XrProviderBinding *bindings,
                          size_t binding_count) {
    if (binding_count == 0u) {
        instance->providers = NULL;
        instance->provider_count = 0u;
        return true;
    }
    instance->providers = xr_calloc(binding_count, sizeof(XrBoundProvider));
    if (!instance->providers)
        return false;
    instance->provider_count = binding_count;
    for (size_t provider = 0; provider < binding_count; ++provider) {
        XrBoundProvider *destination = &instance->providers[provider];
        const XrProviderBinding *source = &bindings[provider];
        destination->contract_id = source->contract_id;
        destination->contract_fingerprint = source->contract_fingerprint;
        destination->behavior_flags = source->behavior_flags;
        destination->operation_count = source->operation_count;
        destination->operations = xr_calloc(source->operation_count, sizeof(XrBoundOperation));
        if (!destination->operations)
            return false;
        for (uint16_t operation = 0; operation < source->operation_count; ++operation) {
            destination->operations[operation].operation_id =
                source->operations[operation].operation_id;
            destination->operations[operation].trampoline_kind =
                source->operations[operation].trampoline_kind;
            if (source->operations[operation].trampoline_kind ==
                XR_PROVIDER_TRAMPOLINE_I64_UNARY)
                destination->operations[operation].entry.i64_unary =
                    source->operations[operation].entry.i64_unary;
            else if (source->operations[operation].trampoline_kind ==
                     XR_PROVIDER_TRAMPOLINE_I64_NULLARY)
                destination->operations[operation].entry.i64_nullary =
                    source->operations[operation].entry.i64_nullary;
            else if (source->operations[operation].trampoline_kind ==
                     XR_PROVIDER_TRAMPOLINE_BOOL_I64_UNARY)
                destination->operations[operation].entry.bool_i64_unary =
                    source->operations[operation].entry.bool_i64_unary;
            else if (source->operations[operation].trampoline_kind ==
                     XR_PROVIDER_TRAMPOLINE_OPTIONAL_I64_PAIR_NULLARY)
                destination->operations[operation].entry.optional_i64_pair_nullary =
                    source->operations[operation].entry.optional_i64_pair_nullary;
            else if (source->operations[operation].trampoline_kind == XR_PROVIDER_TRAMPOLINE_TYPED)
                destination->operations[operation].entry.typed = source->operations[operation].entry.typed;
            else
                destination->operations[operation].entry.output_write =
                    source->operations[operation].entry.output_write;
            destination->operations[operation].context = source->operations[operation].context;
        }
    }
    return true;
}

XrExecutionStatus xr_execution_instance_create(const XrExecutionBindingInput *input,
                                               XrInstance **instance_out,
                                               XrExecutionDiagnostic *diagnostic_out) {
    if (instance_out)
        *instance_out = NULL;
    clear_diagnostic(diagnostic_out);
    if (!input || !instance_out || input->schema_version != XR_EXECUTION_BINDING_SCHEMA_VERSION ||
        input->reserved32 != 0 || !input->program || !input->profile || input->generation == 0)
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_INVALID_INPUT, 0, 0,
                      (XrStableId) {{0}}, (XrStableId) {{0}}, XR_EXECUTION_INVALID_INPUT);
    if (!xr_target_profile_verify(input->profile, NULL, 0) ||
        !xr_target_profile_boundary_abi(input->profile) ||
        !xr_target_profile_runtime_kernel(input->profile))
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_PROFILE, 0, 0, (XrStableId) {{0}},
                      (XrStableId) {{0}}, XR_EXECUTION_PROFILE_REJECTED);
    XrExecutionStatus status = validate_bindings(input, diagnostic_out);
    if (status != XR_EXECUTION_OK)
        return status;

    XrInstance *instance = xr_calloc(1u, sizeof(XrInstance));
    if (!instance)
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_OUT_OF_MEMORY, 0, 0,
                      (XrStableId) {{0}}, (XrStableId) {{0}}, XR_EXECUTION_OUT_OF_MEMORY);
    instance->program = xr_validated_program_retain(input->program);
    instance->profile = xr_target_profile_retain(input->profile);
    instance->generation = input->generation;
    if (!xr_execution_id_compute(input->program, input->profile, &instance->execution_id)) {
        destroy_instance(instance);
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_PROFILE, 0, 0,
                      (XrStableId) {{0}}, (XrStableId) {{0}},
                      XR_EXECUTION_PROFILE_REJECTED);
    }
    atomic_init(&instance->state, XR_INSTANCE_ACTIVE);
    atomic_init(&instance->leases, 0u);
    atomic_init(&instance->lease_lock, false);
    instance->next_lease_ticket = 1u;
    if (!copy_bindings(instance, input->providers, input->provider_count)) {
        destroy_instance(instance);
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_OUT_OF_MEMORY, 0, 0,
                      (XrStableId) {{0}}, (XrStableId) {{0}}, XR_EXECUTION_OUT_OF_MEMORY);
    }
    *instance_out = instance;
    return XR_EXECUTION_OK;
}

XrExecutionStatus xr_execution_instance_create_successor(const XrInstance *retired,
                                                         const XrProviderBinding *providers,
                                                         size_t provider_count,
                                                         XrInstance **instance_out,
                                                         XrExecutionDiagnostic *diagnostic_out) {
    if (instance_out)
        *instance_out = NULL;
    clear_diagnostic(diagnostic_out);
    if (!retired || xr_execution_instance_state(retired) != XR_INSTANCE_RETIRED ||
        retired->generation == UINT64_MAX || !instance_out)
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_GENERATION_STATE, 0, 0,
                      (XrStableId) {{0}}, (XrStableId) {{0}}, XR_EXECUTION_GENERATION_REJECTED);
    XrExecutionBindingInput input = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = retired->program,
        .profile = retired->profile,
        .providers = providers,
        .provider_count = provider_count,
        .generation = retired->generation + 1u,
    };
    return xr_execution_instance_create(&input, instance_out, diagnostic_out);
}

static XrExecutionLeaseTicket *allocate_ticket_locked(XrInstance *instance, bool resource_pin) {
    if (instance->next_lease_ticket == 0u)
        return NULL;
    size_t slot = 0u;
    while (slot < instance->lease_ticket_capacity && instance->lease_tickets[slot].id != 0u)
        ++slot;
    if (slot == instance->lease_ticket_capacity) {
        size_t old_capacity = instance->lease_ticket_capacity;
        size_t new_capacity = old_capacity < 8u ? 8u : old_capacity * 2u;
        if (new_capacity < old_capacity || new_capacity > SIZE_MAX / sizeof(XrExecutionLeaseTicket)) {
            return NULL;
        }
        XrExecutionLeaseTicket *grown =
            xr_realloc(instance->lease_tickets, new_capacity * sizeof(*grown));
        if (!grown) {
            return NULL;
        }
        memset(grown + old_capacity, 0,
               (new_capacity - old_capacity) * sizeof(*grown));
        instance->lease_tickets = grown;
        instance->lease_ticket_capacity = new_capacity;
        slot = old_capacity;
    }
    uint64_t ticket = instance->next_lease_ticket;
    instance->next_lease_ticket = ticket == UINT64_MAX ? 0u : ticket + 1u;
    instance->lease_tickets[slot].id = ticket;
    instance->lease_tickets[slot].in_flight_calls = 0u;
    instance->lease_tickets[slot].resource_pin = resource_pin;
    return &instance->lease_tickets[slot];
}

XrExecutionStatus xr_execution_instance_acquire(XrInstance *instance, XrExecutionLease *lease_out) {
    if (!instance || !lease_out || lease_out->instance || lease_out->ticket != 0u)
        return XR_EXECUTION_INVALID_INPUT;
    lease_lock(instance);
    if (atomic_load_explicit(&instance->state, memory_order_relaxed) != XR_INSTANCE_ACTIVE ||
        instance->next_lease_ticket == 0u) {
        lease_unlock(instance);
        return XR_EXECUTION_GENERATION_REJECTED;
    }
    XrExecutionLeaseTicket *ticket = allocate_ticket_locked(instance, false);
    if (!ticket) {
        lease_unlock(instance);
        return XR_EXECUTION_OUT_OF_MEMORY;
    }
    atomic_fetch_add_explicit(&instance->leases, 1u, memory_order_relaxed);
    lease_out->instance = instance;
    lease_out->ticket = ticket->id;
    lease_unlock(instance);
    return XR_EXECUTION_OK;
}

static XrExecutionStatus resource_pin_acquire(const XrExecutionLease *lease,
                                              XrExecutionResourcePin *pin_out) {
    if (!lease || !lease->instance || !pin_out || pin_out->instance || pin_out->ticket != 0u)
        return XR_EXECUTION_INVALID_INPUT;
    XrInstance *instance = lease->instance;
    lease_lock(instance);
    if (!find_lease_ticket_locked(instance, lease->ticket) || instance->resource_pins == UINT64_MAX) {
        lease_unlock(instance);
        return XR_EXECUTION_GENERATION_REJECTED;
    }
    XrExecutionLeaseTicket *ticket = allocate_ticket_locked(instance, true);
    if (!ticket) {
        lease_unlock(instance);
        return XR_EXECUTION_OUT_OF_MEMORY;
    }
    ++instance->resource_pins;
    pin_out->instance = instance;
    pin_out->ticket = ticket->id;
    lease_unlock(instance);
    return XR_EXECUTION_OK;
}

bool xr_execution_resource_pin_acquire(const XrExecutionLease *lease,
                                       XrExecutionResourcePin *pin_out) {
    return resource_pin_acquire(lease, pin_out) == XR_EXECUTION_OK;
}

bool xr_execution_resource_pin_release(XrExecutionResourcePin *pin) {
    if (!pin || !pin->instance || pin->ticket == 0u)
        return false;
    XrInstance *instance = pin->instance;
    lease_lock(instance);
    XrExecutionLeaseTicket *ticket = find_ticket_locked(instance, pin->ticket, true);
    if (!ticket) {
        lease_unlock(instance);
        return false;
    }
    XR_CHECK(instance->resource_pins != 0u, "resource pin count lost its ticket");
    ticket->id = 0u;
    --instance->resource_pins;
    pin->instance = NULL;
    pin->ticket = 0u;
    lease_unlock(instance);
    return true;
}

XrExecutionStatus xr_execution_resource_adopt(
    const XrExecutionLease *lease, uint16_t type_id, XrStableId resource_id,
    void *payload, void (*destroy)(void *), XrExecutionResource **resource_out) {
    if (!resource_out || *resource_out || !payload || !destroy)
        return XR_EXECUTION_INVALID_INPUT;
    XrExecutionResourcePin pin = {0};
    XrExecutionStatus status = resource_pin_acquire(lease, &pin);
    if (status != XR_EXECUTION_OK)
        return status;
    /* The pin keeps the immutable Program alive even if another holder releases
     * a copied lease after acquisition. It does not extend a language borrow. */
    const XrValidatedType *type = xr_validated_program_type(pin.instance->program, type_id);
    if (!type || type->kind != XR_CORE_IR_TYPE_PROVIDER_RESOURCE ||
        !stable_id_equal(type->resource_id, resource_id)) {
        XR_CHECK(xr_execution_resource_pin_release(&pin), "resource admission lost its pin");
        return XR_EXECUTION_INVALID_INPUT;
    }
    XrExecutionResource *resource = xr_calloc(1u, sizeof(*resource));
    if (!resource) {
        XR_CHECK(xr_execution_resource_pin_release(&pin), "resource allocation lost its pin");
        return XR_EXECUTION_OUT_OF_MEMORY;
    }
    resource->pin = pin;
    resource->resource_id = resource_id;
    resource->payload = payload;
    resource->destroy = destroy;
    *resource_out = resource;
    return XR_EXECUTION_OK;
}

bool xr_execution_resource_borrow(const XrExecutionLease *lease,
                                   const XrExecutionResource *resource,
                                   XrStableId resource_id, void **payload_out) {
    if (!lease || !resource || !payload_out || !resource->payload ||
        lease->instance != resource->pin.instance ||
        !stable_id_equal(resource->resource_id, resource_id) ||
        !xr_execution_lease_is_valid(lease))
        return false;
    *payload_out = resource->payload;
    return true;
}

void xr_execution_resource_free(XrExecutionResource **resource) {
    if (!resource || !*resource)
        return;
    XrExecutionResource *owned = *resource;
    *resource = NULL;
    void *payload = owned->payload;
    owned->payload = NULL;
    owned->destroy(payload);
    XR_CHECK(xr_execution_resource_pin_release(&owned->pin), "resource finalizer lost its pin");
    xr_free(owned);
}

#include "xr_execution_provider_typed.inc.c"

bool xr_execution_lease_release(XrExecutionLease *lease) {
    if (!lease || !lease->instance || lease->ticket == 0u)
        return false;
    XrInstance *instance = lease->instance;
    lease_lock(instance);
    XrExecutionLeaseTicket *ticket = find_lease_ticket_locked(instance, lease->ticket);
    if (!ticket || ticket->in_flight_calls != 0u) {
        lease_unlock(instance);
        return false;
    }
    if (instance->initialization_ticket == lease->ticket) {
        instance->initialization_failed = true;
        instance->initialization_ticket = 0u;
        instance->initializer_active = false;
    }
    ticket->id = 0u;
    atomic_fetch_sub_explicit(&instance->leases, 1u, memory_order_relaxed);
    XrExecutionRuntimeState state = take_drained_state_locked(instance);
    lease_unlock(instance);
    lease->instance = NULL;
    lease->ticket = 0u;
    destroy_drained_state(instance, state);
    return true;
}

XrExecutionStateBindingResult xr_execution_lease_bind_state(
    const XrExecutionLease *lease, const void *layout_key, void *candidate,
    void (*destroy)(void *), void **state_out) {
    if (state_out)
        *state_out = NULL;
    if (!lease || !lease->instance || lease->ticket == 0u || !layout_key || !state_out ||
        (candidate && !destroy) || (!candidate && destroy))
        return XR_EXECUTION_STATE_INVALID;
    XrInstance *instance = lease->instance;
    lease_lock(instance);
    XrExecutionStateBindingResult result = XR_EXECUTION_STATE_INVALID;
    if (!lease_ticket_is_active_locked(instance, lease->ticket))
        goto done;
    if (instance->runtime_state.value) {
        if (instance->runtime_state.layout_key != layout_key) {
            result = XR_EXECUTION_STATE_INCOMPATIBLE;
        } else {
            *state_out = instance->runtime_state.value;
            result = XR_EXECUTION_STATE_PRESENT;
        }
    } else if (!candidate) {
        result = XR_EXECUTION_STATE_EMPTY;
    } else {
        instance->runtime_state = (XrExecutionRuntimeState) {layout_key, candidate, destroy};
        *state_out = candidate;
        result = XR_EXECUTION_STATE_ADOPTED;
    }
done:
    lease_unlock(instance);
    return result;
}

bool xr_execution_lease_is_valid(const XrExecutionLease *lease) {
    if (!lease || !lease->instance || lease->ticket == 0u)
        return false;
    XrInstance *instance = lease->instance;
    lease_lock(instance);
    bool valid = lease_ticket_is_active_locked(instance, lease->ticket);
    lease_unlock(instance);
    return valid;
}

XrExecutionInitializationStatus xr_execution_lease_initialization_next(
    const XrExecutionLease *lease, XrExecutionInitializationStep *step_out) {
    if (step_out)
        *step_out = (XrExecutionInitializationStep) {UINT32_MAX, UINT32_MAX};
    if (!lease || !lease->instance || lease->ticket == 0u || !step_out)
        return XR_EXECUTION_INITIALIZATION_INVALID;
    XrInstance *instance = lease->instance;
    lease_lock(instance);
    XrExecutionInitializationStatus status = XR_EXECUTION_INITIALIZATION_INVALID;
    if (!lease_ticket_is_active_locked(instance, lease->ticket))
        goto done;
    if (instance->initialization_failed) {
        status = XR_EXECUTION_INITIALIZATION_FAILED;
    } else if (instance->initialized_modules == instance->program->module_count) {
        status = XR_EXECUTION_INITIALIZATION_COMPLETE;
    } else if (instance->initializer_active ||
               (instance->initialization_ticket != 0u &&
                instance->initialization_ticket != lease->ticket)) {
        status = XR_EXECUTION_INITIALIZATION_WAIT;
    } else {
        instance->initialization_ticket = lease->ticket;
        instance->initializer_active = true;
        step_out->module_index = instance->initialized_modules;
        step_out->function_id =
            instance->program->modules[instance->initialized_modules].initializer;
        status = XR_EXECUTION_INITIALIZATION_RUN;
    }
done:
    lease_unlock(instance);
    return status;
}

bool xr_execution_lease_initialization_finish(const XrExecutionLease *lease,
                                              uint32_t module_index, bool succeeded) {
    if (!lease || !lease->instance || lease->ticket == 0u)
        return false;
    XrInstance *instance = lease->instance;
    lease_lock(instance);
    bool valid = lease_ticket_is_active_locked(instance, lease->ticket) &&
                 instance->initialization_ticket == lease->ticket &&
                 instance->initializer_active && !instance->initialization_failed &&
                 module_index == instance->initialized_modules &&
                 module_index < instance->program->module_count;
    if (valid) {
        instance->initializer_active = false;
        if (succeeded)
            ++instance->initialized_modules;
        else
            instance->initialization_failed = true;
        if (!succeeded || instance->initialized_modules == instance->program->module_count)
            instance->initialization_ticket = 0u;
    }
    lease_unlock(instance);
    return valid;
}

XrValidatedProgram *xr_execution_lease_retain_program(const XrExecutionLease *lease) {
    if (!lease || !lease->instance || lease->ticket == 0u)
        return NULL;
    XrInstance *instance = lease->instance;
    lease_lock(instance);
    XrValidatedProgram *program =
        lease_ticket_is_active_locked(instance, lease->ticket)
            ? xr_validated_program_retain(instance->program)
            : NULL;
    lease_unlock(instance);
    return program;
}

XrTargetProfile *xr_execution_lease_retain_profile(const XrExecutionLease *lease) {
    if (!lease || !lease->instance || lease->ticket == 0u)
        return NULL;
    XrInstance *instance = lease->instance;
    lease_lock(instance);
    XrTargetProfile *profile = lease_ticket_is_active_locked(instance, lease->ticket)
                                   ? xr_target_profile_retain(instance->profile)
                                   : NULL;
    lease_unlock(instance);
    return profile;
}

XrExecutionProviderCallResult xr_execution_lease_provider_call_i64_unary(
    const XrExecutionLease *lease, uint32_t requirement_index, uint32_t operation_index,
    int64_t argument, int64_t *result_out) {
    if (result_out)
        *result_out = 0;
    if (!lease || !lease->instance || lease->ticket == 0u)
        return XR_EXECUTION_PROVIDER_CALL_INVALID_LEASE;
    if (!result_out)
        return XR_EXECUTION_PROVIDER_CALL_INVALID_REFERENCE;
    XrInstance *instance = lease->instance;
    const uint64_t lease_ticket = lease->ticket;
    lease_lock(instance);
    XrExecutionLeaseTicket *ticket = find_lease_ticket_locked(instance, lease_ticket);
    if (!ticket) {
        lease_unlock(instance);
        return XR_EXECUTION_PROVIDER_CALL_INVALID_LEASE;
    }
    XrProgramProviderRequirementView requirement = {0};
    if (requirement_index >= instance->provider_count ||
        !xr_validated_program_provider_requirement(instance->program, requirement_index,
                                                   &requirement) ||
        operation_index >= requirement.operation_count) {
        lease_unlock(instance);
        return XR_EXECUTION_PROVIDER_CALL_INVALID_REFERENCE;
    }
    XrBoundProvider *provider = &instance->providers[requirement_index];
    if (operation_index >= provider->operation_count ||
        !stable_id_equal(provider->contract_id, requirement.contract_id) ||
        !stable_id_equal(provider->operations[operation_index].operation_id,
                         requirement.operations[operation_index].operation_id) ||
        provider->operations[operation_index].trampoline_kind != XR_PROVIDER_TRAMPOLINE_I64_UNARY ||
        !provider->operations[operation_index].entry.i64_unary ||
        ticket->in_flight_calls == UINT32_MAX) {
        lease_unlock(instance);
        return XR_EXECUTION_PROVIDER_CALL_INVALID_REFERENCE;
    }
    XrProviderI64UnaryEntry entry =
        provider->operations[operation_index].entry.i64_unary;
    void *entry_context = provider->operations[operation_index].context;
    ++ticket->in_flight_calls;
    lease_unlock(instance);

    int64_t provider_result = 0;
    XrProviderCallStatus status = entry(entry_context, argument, &provider_result);

    lease_lock(instance);
    ticket = find_lease_ticket_locked(instance, lease_ticket);
    XR_CHECK(ticket && ticket->in_flight_calls != 0u,
             "provider call lost its execution lease pin");
    --ticket->in_flight_calls;
    lease_unlock(instance);
    if (status != XR_PROVIDER_CALL_OK)
        return XR_EXECUTION_PROVIDER_CALL_FAILED;
    *result_out = provider_result;
    return XR_EXECUTION_PROVIDER_CALL_OK;
}

XrExecutionProviderCallResult xr_execution_lease_provider_call_i64_nullary(
    const XrExecutionLease *lease, uint32_t requirement_index, uint32_t operation_index,
    int64_t *result_out) {
    if (result_out)
        *result_out = 0;
    if (!lease || !lease->instance || lease->ticket == 0u)
        return XR_EXECUTION_PROVIDER_CALL_INVALID_LEASE;
    if (!result_out)
        return XR_EXECUTION_PROVIDER_CALL_INVALID_REFERENCE;
    XrInstance *instance = lease->instance;
    const uint64_t lease_ticket = lease->ticket;
    lease_lock(instance);
    XrExecutionLeaseTicket *ticket = find_lease_ticket_locked(instance, lease_ticket);
    if (!ticket) {
        lease_unlock(instance);
        return XR_EXECUTION_PROVIDER_CALL_INVALID_LEASE;
    }
    XrProgramProviderRequirementView requirement = {0};
    if (requirement_index >= instance->provider_count ||
        !xr_validated_program_provider_requirement(instance->program, requirement_index,
                                                   &requirement) ||
        operation_index >= requirement.operation_count) {
        lease_unlock(instance);
        return XR_EXECUTION_PROVIDER_CALL_INVALID_REFERENCE;
    }
    XrBoundProvider *provider = &instance->providers[requirement_index];
    if (operation_index >= provider->operation_count ||
        !stable_id_equal(provider->contract_id, requirement.contract_id) ||
        !stable_id_equal(provider->operations[operation_index].operation_id,
                         requirement.operations[operation_index].operation_id) ||
        provider->operations[operation_index].trampoline_kind !=
            XR_PROVIDER_TRAMPOLINE_I64_NULLARY ||
        !provider->operations[operation_index].entry.i64_nullary ||
        ticket->in_flight_calls == UINT32_MAX) {
        lease_unlock(instance);
        return XR_EXECUTION_PROVIDER_CALL_INVALID_REFERENCE;
    }
    XrProviderI64NullaryEntry entry =
        provider->operations[operation_index].entry.i64_nullary;
    void *entry_context = provider->operations[operation_index].context;
    ++ticket->in_flight_calls;
    lease_unlock(instance);

    int64_t provider_result = 0;
    XrProviderCallStatus status = entry(entry_context, &provider_result);

    lease_lock(instance);
    ticket = find_lease_ticket_locked(instance, lease_ticket);
    XR_CHECK(ticket && ticket->in_flight_calls != 0u,
             "nullary provider call lost its execution lease pin");
    --ticket->in_flight_calls;
    lease_unlock(instance);
    if (status != XR_PROVIDER_CALL_OK)
        return XR_EXECUTION_PROVIDER_CALL_FAILED;
    *result_out = provider_result;
    return XR_EXECUTION_PROVIDER_CALL_OK;
}

XrExecutionProviderCallResult xr_execution_lease_provider_call_bool_i64_unary(
    const XrExecutionLease *lease, uint32_t requirement_index, uint32_t operation_index,
    int64_t argument, bool *result_out) {
    if (result_out)
        *result_out = false;
    if (!lease || !lease->instance || lease->ticket == 0u)
        return XR_EXECUTION_PROVIDER_CALL_INVALID_LEASE;
    if (!result_out)
        return XR_EXECUTION_PROVIDER_CALL_INVALID_REFERENCE;
    XrInstance *instance = lease->instance;
    const uint64_t lease_ticket = lease->ticket;
    lease_lock(instance);
    XrExecutionLeaseTicket *ticket = find_lease_ticket_locked(instance, lease_ticket);
    if (!ticket) {
        lease_unlock(instance);
        return XR_EXECUTION_PROVIDER_CALL_INVALID_LEASE;
    }
    XrProgramProviderRequirementView requirement = {0};
    if (requirement_index >= instance->provider_count ||
        !xr_validated_program_provider_requirement(instance->program, requirement_index,
                                                   &requirement) ||
        operation_index >= requirement.operation_count) {
        lease_unlock(instance);
        return XR_EXECUTION_PROVIDER_CALL_INVALID_REFERENCE;
    }
    XrBoundProvider *provider = &instance->providers[requirement_index];
    if (operation_index >= provider->operation_count ||
        !stable_id_equal(provider->contract_id, requirement.contract_id) ||
        !stable_id_equal(provider->operations[operation_index].operation_id,
                         requirement.operations[operation_index].operation_id) ||
        provider->operations[operation_index].trampoline_kind !=
            XR_PROVIDER_TRAMPOLINE_BOOL_I64_UNARY ||
        !provider->operations[operation_index].entry.bool_i64_unary ||
        ticket->in_flight_calls == UINT32_MAX) {
        lease_unlock(instance);
        return XR_EXECUTION_PROVIDER_CALL_INVALID_REFERENCE;
    }
    XrProviderBoolI64UnaryEntry entry =
        provider->operations[operation_index].entry.bool_i64_unary;
    void *entry_context = provider->operations[operation_index].context;
    ++ticket->in_flight_calls;
    lease_unlock(instance);

    bool provider_result = false;
    XrProviderCallStatus status = entry(entry_context, argument, &provider_result);

    lease_lock(instance);
    ticket = find_lease_ticket_locked(instance, lease_ticket);
    XR_CHECK(ticket && ticket->in_flight_calls != 0u,
             "bool unary provider call lost its execution lease pin");
    --ticket->in_flight_calls;
    lease_unlock(instance);
    if (status != XR_PROVIDER_CALL_OK)
        return XR_EXECUTION_PROVIDER_CALL_FAILED;
    *result_out = provider_result;
    return XR_EXECUTION_PROVIDER_CALL_OK;
}

XrExecutionProviderCallResult xr_execution_lease_provider_call_optional_i64_pair_nullary(
    const XrExecutionLease *lease, uint32_t requirement_index, uint32_t operation_index,
    bool *present_out, int64_t *first_out, int64_t *second_out) {
    if (present_out)
        *present_out = false;
    if (first_out)
        *first_out = 0;
    if (second_out)
        *second_out = 0;
    if (!lease || !lease->instance || lease->ticket == 0u)
        return XR_EXECUTION_PROVIDER_CALL_INVALID_LEASE;
    if (!present_out || !first_out || !second_out)
        return XR_EXECUTION_PROVIDER_CALL_INVALID_REFERENCE;
    XrInstance *instance = lease->instance;
    const uint64_t lease_ticket = lease->ticket;
    lease_lock(instance);
    XrExecutionLeaseTicket *ticket = find_lease_ticket_locked(instance, lease_ticket);
    if (!ticket) {
        lease_unlock(instance);
        return XR_EXECUTION_PROVIDER_CALL_INVALID_LEASE;
    }
    XrProgramProviderRequirementView requirement = {0};
    if (requirement_index >= instance->provider_count ||
        !xr_validated_program_provider_requirement(instance->program, requirement_index,
                                                   &requirement) ||
        operation_index >= requirement.operation_count) {
        lease_unlock(instance);
        return XR_EXECUTION_PROVIDER_CALL_INVALID_REFERENCE;
    }
    XrBoundProvider *provider = &instance->providers[requirement_index];
    if (operation_index >= provider->operation_count ||
        !stable_id_equal(provider->contract_id, requirement.contract_id) ||
        !stable_id_equal(provider->operations[operation_index].operation_id,
                         requirement.operations[operation_index].operation_id) ||
        provider->operations[operation_index].trampoline_kind !=
            XR_PROVIDER_TRAMPOLINE_OPTIONAL_I64_PAIR_NULLARY ||
        !provider->operations[operation_index].entry.optional_i64_pair_nullary ||
        ticket->in_flight_calls == UINT32_MAX) {
        lease_unlock(instance);
        return XR_EXECUTION_PROVIDER_CALL_INVALID_REFERENCE;
    }
    XrProviderOptionalI64PairNullaryEntry entry =
        provider->operations[operation_index].entry.optional_i64_pair_nullary;
    void *entry_context = provider->operations[operation_index].context;
    ++ticket->in_flight_calls;
    lease_unlock(instance);

    bool provider_present = false;
    int64_t provider_first = 0;
    int64_t provider_second = 0;
    XrProviderCallStatus status =
        entry(entry_context, &provider_present, &provider_first, &provider_second);

    lease_lock(instance);
    ticket = find_lease_ticket_locked(instance, lease_ticket);
    XR_CHECK(ticket && ticket->in_flight_calls != 0u,
             "optional pair provider call lost its execution lease pin");
    --ticket->in_flight_calls;
    lease_unlock(instance);
    if (status != XR_PROVIDER_CALL_OK)
        return XR_EXECUTION_PROVIDER_CALL_FAILED;
    *present_out = provider_present;
    if (provider_present) {
        *first_out = provider_first;
        *second_out = provider_second;
    }
    return XR_EXECUTION_PROVIDER_CALL_OK;
}

XrExecutionProviderCallResult xr_execution_lease_provider_output_write(
    const XrExecutionLease *lease, uint32_t requirement_index, uint32_t operation_index,
    const uint8_t *bytes, size_t size) {
    if (!lease || !lease->instance || lease->ticket == 0u)
        return XR_EXECUTION_PROVIDER_CALL_INVALID_LEASE;
    if (!bytes && size != 0u)
        return XR_EXECUTION_PROVIDER_CALL_INVALID_REFERENCE;
    XrInstance *instance = lease->instance;
    const uint64_t lease_ticket = lease->ticket;
    lease_lock(instance);
    XrExecutionLeaseTicket *ticket = find_lease_ticket_locked(instance, lease_ticket);
    if (!ticket) {
        lease_unlock(instance);
        return XR_EXECUTION_PROVIDER_CALL_INVALID_LEASE;
    }
    XrProgramProviderRequirementView requirement = {0};
    if (requirement_index >= instance->provider_count ||
        !xr_validated_program_provider_requirement(instance->program, requirement_index,
                                                   &requirement) ||
        operation_index >= requirement.operation_count) {
        lease_unlock(instance);
        return XR_EXECUTION_PROVIDER_CALL_INVALID_REFERENCE;
    }
    XrBoundProvider *provider = &instance->providers[requirement_index];
    if (operation_index >= provider->operation_count ||
        !stable_id_equal(provider->contract_id, requirement.contract_id) ||
        !stable_id_equal(provider->operations[operation_index].operation_id,
                         requirement.operations[operation_index].operation_id) ||
        provider->operations[operation_index].trampoline_kind !=
            XR_PROVIDER_TRAMPOLINE_OUTPUT_WRITE ||
        !provider->operations[operation_index].entry.output_write ||
        ticket->in_flight_calls == UINT32_MAX) {
        lease_unlock(instance);
        return XR_EXECUTION_PROVIDER_CALL_INVALID_REFERENCE;
    }
    XrProviderOutputWriteEntry entry = provider->operations[operation_index].entry.output_write;
    void *entry_context = provider->operations[operation_index].context;
    ++ticket->in_flight_calls;
    lease_unlock(instance);

    XrProviderCallStatus status = entry(entry_context, bytes, size);

    lease_lock(instance);
    ticket = find_lease_ticket_locked(instance, lease_ticket);
    XR_CHECK(ticket && ticket->in_flight_calls != 0u,
             "provider output lost its execution lease pin");
    --ticket->in_flight_calls;
    lease_unlock(instance);
    return status == XR_PROVIDER_CALL_OK ? XR_EXECUTION_PROVIDER_CALL_OK
                                         : XR_EXECUTION_PROVIDER_CALL_FAILED;
}

XrExecutionStatus xr_execution_instance_begin_drain(XrInstance *instance,
                                                    XrExecutionDiagnostic *diagnostic_out) {
    clear_diagnostic(diagnostic_out);
    if (!instance)
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_INVALID_INPUT, 0, 0,
                      (XrStableId) {{0}}, (XrStableId) {{0}}, XR_EXECUTION_INVALID_INPUT);
    lease_lock(instance);
    if (atomic_load_explicit(&instance->state, memory_order_relaxed) != XR_INSTANCE_ACTIVE) {
        lease_unlock(instance);
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_GENERATION_STATE, 0, 0,
                      (XrStableId) {{0}}, (XrStableId) {{0}}, XR_EXECUTION_GENERATION_REJECTED);
    }
    atomic_store_explicit(&instance->state, XR_INSTANCE_DRAINING, memory_order_release);
    XrExecutionRuntimeState state = take_drained_state_locked(instance);
    lease_unlock(instance);
    destroy_drained_state(instance, state);
    return XR_EXECUTION_OK;
}

XrExecutionStatus xr_execution_instance_retire(XrInstance *instance,
                                               XrExecutionDiagnostic *diagnostic_out) {
    clear_diagnostic(diagnostic_out);
    if (!instance)
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_INVALID_INPUT, 0, 0,
                      (XrStableId) {{0}}, (XrStableId) {{0}}, XR_EXECUTION_INVALID_INPUT);
    lease_lock(instance);
    if (atomic_load_explicit(&instance->leases, memory_order_relaxed) != 0u ||
        instance->resource_pins != 0u || instance->runtime_state.value || instance->state_cleanup_active) {
        lease_unlock(instance);
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_GENERATION_BUSY, 0, 0,
                      (XrStableId) {{0}}, (XrStableId) {{0}}, XR_EXECUTION_GENERATION_REJECTED);
    }
    if (atomic_load_explicit(&instance->state, memory_order_relaxed) != XR_INSTANCE_DRAINING) {
        lease_unlock(instance);
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_GENERATION_STATE, 0, 0,
                      (XrStableId) {{0}}, (XrStableId) {{0}}, XR_EXECUTION_GENERATION_REJECTED);
    }
    atomic_store_explicit(&instance->state, XR_INSTANCE_RETIRED, memory_order_release);
    lease_unlock(instance);
    return XR_EXECUTION_OK;
}

XrExecutionStatus xr_execution_instance_free(XrInstance **instance,
                                             XrExecutionDiagnostic *diagnostic_out) {
    clear_diagnostic(diagnostic_out);
    if (!instance || !*instance)
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_INVALID_INPUT, 0, 0,
                      (XrStableId) {{0}}, (XrStableId) {{0}}, XR_EXECUTION_INVALID_INPUT);
    if (xr_execution_instance_state(*instance) != XR_INSTANCE_RETIRED ||
        xr_execution_instance_lease_count(*instance) != 0u)
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_GENERATION_STATE, 0, 0,
                      (XrStableId) {{0}}, (XrStableId) {{0}}, XR_EXECUTION_GENERATION_REJECTED);
    destroy_instance(*instance);
    *instance = NULL;
    return XR_EXECUTION_OK;
}

XrInstanceState xr_execution_instance_state(const XrInstance *instance) {
    return instance ? (XrInstanceState) atomic_load_explicit(&instance->state, memory_order_acquire)
                    : 0;
}

uint64_t xr_execution_instance_lease_count(const XrInstance *instance) {
    return instance ? atomic_load_explicit(&instance->leases, memory_order_acquire) : 0u;
}

uint64_t xr_execution_instance_generation(const XrInstance *instance) {
    return instance ? instance->generation : 0u;
}

XrExecutionId xr_execution_instance_id(const XrInstance *instance) {
    XrExecutionId zero = {{0}};
    return instance ? instance->execution_id : zero;
}

XrExecutionCacheKey xr_execution_instance_cache_key(const XrInstance *instance) {
    XrExecutionCacheKey key = {0};
    if (instance) {
        key.execution_id = instance->execution_id;
        key.generation = instance->generation;
    }
    return key;
}

const char *xr_execution_status_name(XrExecutionStatus status) {
    switch (status) {
        case XR_EXECUTION_OK:
            return "ok";
        case XR_EXECUTION_INVALID_INPUT:
            return "invalid-input";
        case XR_EXECUTION_PROFILE_REJECTED:
            return "profile-rejected";
        case XR_EXECUTION_PROVIDER_REJECTED:
            return "provider-rejected";
        case XR_EXECUTION_OUT_OF_MEMORY:
            return "out-of-memory";
        case XR_EXECUTION_GENERATION_REJECTED:
            return "generation-rejected";
        default:
            return "unknown";
    }
}

const char *xr_execution_diagnostic_kind_name(XrExecutionDiagnosticKind kind) {
    switch (kind) {
        case XR_EXECUTION_DIAGNOSTIC_NONE:
            return "none";
        case XR_EXECUTION_DIAGNOSTIC_INVALID_INPUT:
            return "invalid-input";
        case XR_EXECUTION_DIAGNOSTIC_PROFILE:
            return "profile";
        case XR_EXECUTION_DIAGNOSTIC_PROVIDER_COUNT:
            return "provider-count";
        case XR_EXECUTION_DIAGNOSTIC_PROVIDER_CONTRACT:
            return "provider-contract";
        case XR_EXECUTION_DIAGNOSTIC_PROVIDER_OPERATION:
            return "provider-operation";
        case XR_EXECUTION_DIAGNOSTIC_PROVIDER_BEHAVIOR:
            return "provider-behavior";
        case XR_EXECUTION_DIAGNOSTIC_PROVIDER_ABI:
            return "provider-abi";
        case XR_EXECUTION_DIAGNOSTIC_OUT_OF_MEMORY:
            return "out-of-memory";
        case XR_EXECUTION_DIAGNOSTIC_GENERATION_STATE:
            return "generation-state";
        case XR_EXECUTION_DIAGNOSTIC_GENERATION_BUSY:
            return "generation-busy";
        default:
            return "unknown";
    }
}
