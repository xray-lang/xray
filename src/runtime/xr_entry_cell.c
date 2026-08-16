/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_entry_cell.c - Runtime-owned typed entry indirection
 */

#include "xr_entry_cell.h"
#include "../base/xsha256.h"
#include "../plan/semantic/xr_semantic_ids.h"
#include "../plan/target/xr_target_instruction_verify.h"
#include "../plan/target/xr_target_entry_abi.h"
#include "../plan/target/xr_target_plan.h"
#include "../plan/target/xr_target_profile.h"
#include "../vm/xr_typed_dispatch.h"
#include "../vm/xr_vm_entry_adapter.h"
#include "xr_module_generation_internal.h"
#include <stdio.h>
#include <string.h>

enum {
    XR_ENTRY_TOKEN_EMPTY = 0,
    XR_ENTRY_TOKEN_LIVE = 1,
    XR_ENTRY_TOKEN_RELEASING = 2,
    XR_ENTRY_TOKEN_RELEASED = 3,
};

static bool fail(char *diagnostic, size_t diagnostic_size, const char *code,
                 const char *detail) {
    if (diagnostic && diagnostic_size)
        snprintf(diagnostic, diagnostic_size, "%s: %s", code, detail);
    return false;
}

static bool bytes_are_zero(const uint8_t *bytes, size_t size) {
    uint8_t combined = 0;
    for (size_t i = 0; i < size; i++)
        combined |= bytes[i];
    return combined == 0;
}

static void hash_u32(XrSHA256Context *context, uint32_t value) {
    uint8_t bytes[4] = {
        (uint8_t) (value >> 24), (uint8_t) (value >> 16),
        (uint8_t) (value >> 8), (uint8_t) value,
    };
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void hash_fingerprint(XrSHA256Context *context,
                             XrFingerprint fingerprint) {
    xr_sha256_update(context, fingerprint.bytes, sizeof(fingerprint.bytes));
}

static bool exact_i64_slot(const XrTargetPlan *plan, uint32_t function,
                           uint32_t slot_index) {
    uint32_t slot_count = 0u;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(plan, &slot_count);
    const XrTargetSlotRecord *slot =
        slots != NULL && slot_index < slot_count ? &slots[slot_index] : NULL;
    const XrTargetMachineRepRecord *register_rep =
        slot ? xr_target_plan_machine_rep(plan, slot->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        slot ? xr_target_plan_machine_rep(plan, slot->memory_rep) : NULL;
    return slot && register_rep && memory_rep && slot->function == function &&
           slot->size == 8 && slot->root_kind == XR_TARGET_ROOT_NONE &&
           slot->ownership == XR_TARGET_OWNERSHIP_TRIVIAL &&
           register_rep->kind == XR_MACHINE_REP_I64 &&
           register_rep->register_bits == 64 &&
           register_rep->signedness == XR_TARGET_SIGN_SIGNED &&
           register_rep->root_kind == XR_TARGET_ROOT_NONE &&
           register_rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL &&
           memory_rep->kind == XR_MACHINE_REP_I64 &&
           memory_rep->memory_size == 8 &&
           memory_rep->signedness == XR_TARGET_SIGN_SIGNED &&
           memory_rep->root_kind == XR_TARGET_ROOT_NONE &&
           memory_rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL;
}

static bool derive_entry_abi(const XrTargetPlan *plan, uint32_t function,
                             XrEntryAbi *abi, char *diagnostic,
                             size_t diagnostic_size) {
    if (!plan || !abi)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5004",
                    "entry ABI derivation requires a plan and result");
    char verification[512] = {0};
    uint32_t adapter_count = 0;
    xr_target_plan_adapters(plan, &adapter_count);
    if (!xr_target_plan_is_verified(plan) ||
        !xr_target_plan_fingerprint_is_intact(plan) ||
        !xr_target_instruction_program_verify(plan, verification,
                                               sizeof(verification)) ||
        adapter_count != 0 ||
        xr_target_plan_function_execution_family_mask(plan, function) !=
            XR_TARGET_EXECUTION_SCALAR_I64_CLOSED)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5004",
                    "entry requires a verified identity-adapted scalar i64 function");

    uint32_t instruction_count = 0;
    const XrTargetInstructionRecord *instructions =
        xr_target_plan_function_instructions(plan, function,
                                             &instruction_count);
    uint64_t parameter_mask = 0;
    uint32_t parameter_count = 0;
    uint32_t return_count = 0;
    for (uint32_t i = 0; instructions && i < instruction_count; i++) {
        const XrTargetInstructionRecord *row = &instructions[i];
        if (row->opcode == XR_TARGET_INSTRUCTION_PARAM_I64) {
            if (row->immediate_bits >= XR_TARGET_INSTRUCTION_MAX_PARAMETERS ||
                (parameter_mask & (UINT64_C(1) << row->immediate_bits)) != 0 ||
                !exact_i64_slot(plan, function, row->result_slot))
                return fail(diagnostic, diagnostic_size, "XR_EXEC_5004",
                            "entry parameter ABI is not exact signed i64");
            parameter_mask |= UINT64_C(1) << row->immediate_bits;
            parameter_count++;
        } else if (row->opcode == XR_TARGET_INSTRUCTION_RETURN_I64) {
            if (!exact_i64_slot(plan, function, row->operand_slots[0]))
                return fail(diagnostic, diagnostic_size, "XR_EXEC_5004",
                            "entry result ABI is not exact signed i64");
            return_count++;
        }
    }
    uint64_t expected_mask = parameter_count == 64
                                 ? UINT64_MAX
                                 : (UINT64_C(1) << parameter_count) - 1u;
    const XrTargetProfile *profile = xr_target_plan_profile(plan);
    const XrTargetMachineFacts *machine =
        profile ? xr_target_profile_machine_facts(profile) : NULL;
    if (!instructions || !instruction_count || !return_count ||
        parameter_count > UINT16_MAX || parameter_mask != expected_mask ||
        !profile || !machine)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5004",
                    "entry has no complete canonical scalar i64 signature");

    memset(abi, 0, sizeof(*abi));
    abi->schema_version = XR_ENTRY_ABI_SCHEMA_VERSION;
    abi->parameter_count = (uint16_t) parameter_count;
    abi->value_kind = XR_TARGET_ENTRY_VALUE_EXACT_I64;
    abi->native_abi = machine->native_abi;
    abi->target_data_layout = machine->data_layout.stable_hash;
    abi->target_profile_fingerprint = xr_target_profile_fingerprint(profile);
    XrTargetEntryAbiFacts facts = {
        .schema_version = abi->schema_version,
        .parameter_count = abi->parameter_count,
        .native_abi = abi->native_abi,
        .value_kind = abi->value_kind,
        .target_data_layout = abi->target_data_layout,
        .target_profile_fingerprint = abi->target_profile_fingerprint,
    };
    return xr_target_entry_abi_fingerprint(&facts, &abi->fingerprint);
}

static XrFingerprint binding_fingerprint(
    const XrEntryCellRegistration *registration,
    const XrEntryCellExpectation *expectation) {
    static const uint8_t domain[] = "xray-entry-binding-v1\0";
    XrSHA256Context context;
    XrFingerprint fingerprint;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    hash_u32(&context, registration->function);
    hash_u32(&context, registration->executor_kind);
    hash_fingerprint(&context, expectation->abi.fingerprint);
    hash_fingerprint(&context, expectation->adapter_fingerprint);
    hash_fingerprint(&context, expectation->target_plan_fingerprint);
    hash_fingerprint(&context, expectation->generation_fingerprint);
    xr_sha256_update(&context, registration->native_entry_identity.bytes,
                     sizeof(registration->native_entry_identity.bytes));
    xr_sha256_final(&context, fingerprint.bytes);
    return fingerprint;
}

static bool registration_shape_is_exact(
    const XrEntryCellRegistration *registration) {
    if (!registration || !registration->generation ||
        !registration->verified_plan ||
        registration->executor_kind <= XR_ENTRY_EXECUTOR_INVALID ||
        registration->executor_kind >= XR_ENTRY_EXECUTOR_KIND_COUNT)
        return false;
    bool native_identity_zero = bytes_are_zero(
        registration->native_entry_identity.bytes,
        sizeof(registration->native_entry_identity.bytes));
    if (registration->executor_kind == XR_ENTRY_EXECUTOR_TYPED_VM)
        return !registration->native_entry && !registration->native_context &&
               native_identity_zero;
    return registration->executor_kind == XR_ENTRY_EXECUTOR_NATIVE_I64 &&
           registration->native_entry && !native_identity_zero;
}

static bool make_binding(const XrEntryCellRegistration *registration,
                         XrEntryCellBinding *binding, char *diagnostic,
                         size_t diagnostic_size) {
    if (!registration_shape_is_exact(registration) || !binding ||
        registration->generation->plan != registration->verified_plan)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5004",
                    "entry registration does not bind one verified generation plan");
    XrEntryAbi abi;
    if (!derive_entry_abi(registration->verified_plan, registration->function,
                          &abi, diagnostic, diagnostic_size))
        return false;
    if (!xr_module_generation_pin_acquire(
            registration->generation, XR_MODULE_GENERATION_STATIC_ROOT,
            diagnostic, diagnostic_size))
        return false;

    XrModuleGenerationSnapshot snapshot;
    bool exact = xr_module_generation_snapshot(registration->generation,
                                               &snapshot) &&
                 !snapshot.poisoned && !snapshot.rollback_requested &&
                 memcmp(snapshot.identity.target_plan_fingerprint,
                        xr_target_plan_fingerprint(
                            registration->verified_plan).bytes,
                        XR_RUNTIME_GENERATION_FINGERPRINT_SIZE) == 0 &&
                 !bytes_are_zero(snapshot.identity.generation_fingerprint,
                                 XR_RUNTIME_GENERATION_FINGERPRINT_SIZE);
    if (!exact) {
        char nested[256] = {0};
        xr_module_generation_pin_release(
            registration->generation, XR_MODULE_GENERATION_STATIC_ROOT,
            nested, sizeof(nested));
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5008",
                    "entry registration generation identity changed");
    }

    memset(binding, 0, sizeof(*binding));
    binding->generation = registration->generation;
    binding->plan = registration->verified_plan;
    binding->function = registration->function;
    binding->native_entry_identity = registration->native_entry_identity;
    binding->native_entry = registration->native_entry;
    binding->native_context = registration->native_context;
    binding->expectation.abi = abi;
    binding->expectation.executor_kind = registration->executor_kind;
    binding->expectation.adapter_kind = XR_ENTRY_ADAPTER_IDENTITY;
    if (!xr_target_entry_identity_adapter_fingerprint(
            &abi.fingerprint,
            &binding->expectation.adapter_fingerprint)) {
        char nested[256] = {0};
        xr_module_generation_pin_release(
            registration->generation, XR_MODULE_GENERATION_STATIC_ROOT,
            nested, sizeof(nested));
        memset(binding, 0, sizeof(*binding));
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5004",
                    "entry identity adapter fingerprint is unavailable");
    }
    binding->expectation.target_plan_fingerprint =
        xr_target_plan_fingerprint(registration->verified_plan);
    memcpy(binding->expectation.generation_fingerprint.bytes,
           snapshot.identity.generation_fingerprint,
           sizeof(binding->expectation.generation_fingerprint.bytes));
    binding->expectation.binding_fingerprint =
        binding_fingerprint(registration, &binding->expectation);
    return true;
}

bool xr_entry_cell_init(XrEntryCell *cell) {
    if (!cell)
        return false;
    memset(cell, 0, sizeof(*cell));
    xr_mutex_init(&cell->gate);
    cell->initialized = true;
    return true;
}

static bool release_binding_pin(XrEntryCellBinding *binding, char *diagnostic,
                                size_t diagnostic_size) {
    if (!binding->generation)
        return true;
    if (!xr_module_generation_pin_release(
            binding->generation, XR_MODULE_GENERATION_STATIC_ROOT,
            diagnostic, diagnostic_size))
        return false;
    memset(binding, 0, sizeof(*binding));
    return true;
}

bool xr_entry_cell_bind(XrEntryCell *cell,
                        const XrEntryCellRegistration *registration,
                        XrEntryCellExpectation *expectation, char *diagnostic,
                        size_t diagnostic_size) {
    if (expectation)
        memset(expectation, 0, sizeof(*expectation));
    if (!cell || !cell->initialized || !expectation)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5004",
                    "entry bind requires an initialized cell and expectation");
    XrEntryCellBinding next;
    if (!make_binding(registration, &next, diagnostic, diagnostic_size))
        return false;

    xr_mutex_lock(&cell->gate);
    if (!release_binding_pin(&cell->binding, diagnostic, diagnostic_size)) {
        xr_mutex_unlock(&cell->gate);
        char nested[256] = {0};
        release_binding_pin(&next, nested, sizeof(nested));
        return false;
    }
    cell->binding = next;
    *expectation = next.expectation;
    xr_mutex_unlock(&cell->gate);
    return true;
}

bool xr_entry_cell_clear(XrEntryCell *cell, char *diagnostic,
                         size_t diagnostic_size) {
    if (!cell || !cell->initialized)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5004",
                    "entry clear requires an initialized cell");
    xr_mutex_lock(&cell->gate);
    bool released = release_binding_pin(&cell->binding, diagnostic,
                                        diagnostic_size);
    xr_mutex_unlock(&cell->gate);
    return released;
}

bool xr_entry_cell_dispose(XrEntryCell *cell, char *diagnostic,
                           size_t diagnostic_size) {
    if (!cell || !cell->initialized)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5004",
                    "entry dispose requires an initialized cell");
    xr_mutex_lock(&cell->gate);
    bool empty = cell->binding.generation == NULL;
    xr_mutex_unlock(&cell->gate);
    if (!empty)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5006",
                    "entry dispose requires an empty cell");
    xr_mutex_destroy(&cell->gate);
    memset(cell, 0, sizeof(*cell));
    return true;
}

static bool expectation_matches(const XrEntryCellExpectation *expected,
                                const XrEntryCellExpectation *actual) {
    return expected && actual &&
           expected->abi.schema_version == XR_ENTRY_ABI_SCHEMA_VERSION &&
           expected->abi.parameter_count == actual->abi.parameter_count &&
           expected->abi.value_kind == actual->abi.value_kind &&
           expected->abi.reserved8 == 0 && actual->abi.reserved8 == 0 &&
           expected->abi.native_abi == actual->abi.native_abi &&
           expected->abi.reserved16 == 0 && actual->abi.reserved16 == 0 &&
           expected->abi.target_data_layout == actual->abi.target_data_layout &&
           xr_fingerprint_equal(expected->abi.target_profile_fingerprint,
                                actual->abi.target_profile_fingerprint) &&
           xr_fingerprint_equal(expected->abi.fingerprint,
                                actual->abi.fingerprint) &&
           xr_fingerprint_equal(expected->adapter_fingerprint,
                                actual->adapter_fingerprint) &&
           xr_fingerprint_equal(expected->target_plan_fingerprint,
                                actual->target_plan_fingerprint) &&
           xr_fingerprint_equal(expected->generation_fingerprint,
                                actual->generation_fingerprint) &&
           xr_fingerprint_equal(expected->binding_fingerprint,
                                actual->binding_fingerprint) &&
           expected->executor_kind == actual->executor_kind &&
           expected->adapter_kind == XR_ENTRY_ADAPTER_IDENTITY &&
           actual->adapter_kind == XR_ENTRY_ADAPTER_IDENTITY;
}

bool xr_entry_cell_acquire(XrEntryCell *cell,
                           const XrEntryCellExpectation *expectation,
                           XrEntryCallToken *token, char *diagnostic,
                           size_t diagnostic_size) {
    if (token)
        memset(token, 0, sizeof(*token));
    if (!cell || !cell->initialized || !expectation || !token)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5004",
                    "entry acquire requires a cell, expectation, and token");
    xr_mutex_lock(&cell->gate);
    const XrEntryCellBinding *binding = &cell->binding;
    if (!binding->generation || !expectation_matches(
                                    expectation, &binding->expectation)) {
        xr_mutex_unlock(&cell->gate);
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5008",
                    "entry binding identity does not match the call expectation");
    }
    if (!xr_module_generation_pin_acquire(
            binding->generation, XR_MODULE_GENERATION_INFLIGHT_CALL,
            diagnostic, diagnostic_size)) {
        xr_mutex_unlock(&cell->gate);
        return false;
    }
    XrModuleGenerationSnapshot snapshot;
    bool exact = xr_module_generation_snapshot(binding->generation, &snapshot) &&
                 !snapshot.poisoned && !snapshot.rollback_requested &&
                 memcmp(snapshot.identity.generation_fingerprint,
                        expectation->generation_fingerprint.bytes,
                        XR_RUNTIME_GENERATION_FINGERPRINT_SIZE) == 0 &&
                 memcmp(snapshot.identity.target_plan_fingerprint,
                        expectation->target_plan_fingerprint.bytes,
                        XR_RUNTIME_GENERATION_FINGERPRINT_SIZE) == 0;
    if (!exact) {
        char nested[256] = {0};
        xr_module_generation_pin_release(
            binding->generation, XR_MODULE_GENERATION_INFLIGHT_CALL,
            nested, sizeof(nested));
        xr_mutex_unlock(&cell->gate);
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5008",
                    "entry generation identity changed during acquisition");
    }
    token->generation = binding->generation;
    token->plan = binding->plan;
    token->native_entry = binding->native_entry;
    token->native_context = binding->native_context;
    token->plan_fingerprint = expectation->target_plan_fingerprint;
    token->function = binding->function;
    token->executor_kind = binding->expectation.executor_kind;
    atomic_init(&token->release_state, XR_ENTRY_TOKEN_LIVE);
    xr_mutex_unlock(&cell->gate);
    return true;
}

bool xr_entry_call_release(XrEntryCallToken *token, char *diagnostic,
                           size_t diagnostic_size) {
    if (!token)
        return fail(diagnostic, diagnostic_size, "XR_OWN_3003",
                    "entry token is missing");
    unsigned int expected = XR_ENTRY_TOKEN_LIVE;
    if (!atomic_compare_exchange_strong_explicit(
            &token->release_state, &expected, XR_ENTRY_TOKEN_RELEASING,
            memory_order_acq_rel, memory_order_acquire))
        return fail(diagnostic, diagnostic_size, "XR_OWN_3003",
                    "entry token was already released or never acquired");
    bool released = xr_module_generation_pin_release(
        token->generation, XR_MODULE_GENERATION_INFLIGHT_CALL,
        diagnostic, diagnostic_size);
    if (!released) {
        atomic_store_explicit(&token->release_state, XR_ENTRY_TOKEN_LIVE,
                              memory_order_release);
        return false;
    }
    token->generation = NULL;
    token->plan = NULL;
    token->native_entry = NULL;
    token->native_context = NULL;
    atomic_store_explicit(&token->release_state, XR_ENTRY_TOKEN_RELEASED,
                          memory_order_release);
    return true;
}

XrEntryInvokeStatus xr_entry_cell_invoke_i64(
    XrEntryCell *cell, const XrEntryCellExpectation *expectation,
    const int64_t *arguments, uint32_t argument_count, int64_t *result,
    uint32_t *executor_status, char *diagnostic, size_t diagnostic_size) {
    if (result)
        *result = 0;
    if (executor_status)
        *executor_status = 0;
    if (!result || !executor_status || !expectation ||
        argument_count != expectation->abi.parameter_count ||
        (argument_count && !arguments)) {
        fail(diagnostic, diagnostic_size, "XR_EXEC_5004",
             "entry invocation arguments do not match the canonical ABI");
        return XR_ENTRY_INVOKE_INVALID_ARGUMENT;
    }
    XrEntryCallToken token;
    if (!xr_entry_cell_acquire(cell, expectation, &token, diagnostic,
                               diagnostic_size))
        return XR_ENTRY_INVOKE_AUTHORITY_ERROR;

    XrEntryInvokeStatus outcome = XR_ENTRY_INVOKE_NATIVE_ERROR;
    int64_t executed = 0;
    XrVmEntryAdapterI64 adapter;
    if (!xr_typed_entry_adapter_i64_freeze(
            expectation, &token, &adapter, diagnostic, diagnostic_size)) {
        outcome = XR_ENTRY_INVOKE_AUTHORITY_ERROR;
    } else if (adapter.executor_kind == XR_ENTRY_EXECUTOR_TYPED_VM) {
        XrTypedDispatchI64Request request = {
            .verified_plan = token.plan,
            .required_plan_fingerprint = &token.plan_fingerprint,
            .arguments = arguments,
            .result = &executed,
            .decoded_cache = token.generation->decoded_cache,
            .dynamic_entries = &token.generation->dynamic_entries,
            .generation_identity = &token.generation->identity,
            .provider =
                XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE,
            .function = token.function,
            .argument_count = argument_count,
            .use_dynamic_entry_cache = true,
        };
        XrTypedDispatchStatus status =
            xr_typed_dispatch_execute_i64(&request);
        *executor_status = (uint32_t) status;
        outcome = status == XR_TYPED_DISPATCH_OK ? XR_ENTRY_INVOKE_OK
                                                 : XR_ENTRY_INVOKE_VM_ERROR;
    } else if (adapter.executor_kind == XR_ENTRY_EXECUTOR_NATIVE_I64) {
        XrEntryNativeStatus status = xr_typed_entry_adapter_i64_invoke_native(
            &adapter, arguments, argument_count, &executed);
        *executor_status = (uint32_t) status;
        if (status == XR_ENTRY_NATIVE_OK)
            outcome = XR_ENTRY_INVOKE_OK;
        else if (status == XR_ENTRY_NATIVE_CANCELLED)
            outcome = XR_ENTRY_INVOKE_CANCELLED;
        else
            outcome = XR_ENTRY_INVOKE_NATIVE_ERROR;
    }
    if (!xr_entry_call_release(&token, diagnostic, diagnostic_size))
        return XR_ENTRY_INVOKE_RELEASE_ERROR;
    if (outcome == XR_ENTRY_INVOKE_OK)
        *result = executed;
    return outcome;
}
