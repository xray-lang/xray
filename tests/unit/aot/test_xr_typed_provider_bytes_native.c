/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xr_typed_provider_bytes_native.c - The same mutable byte program in VM and native
 */
#include "../execution/xr_typed_provider_bytes_fixture.h"
#include "execution/xr_native_execution.h"
#include "vm/xr_program_vm.h"
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); abort(); } } while (0)
extern const XrBackendNativeDescriptor xr_aot_entry_coroutine_descriptor;

typedef struct ByteHost { unsigned mode, calls; } ByteHost;

static XrProviderCallStatus byte_host_call(void *opaque, const XrProviderValuePack *arguments,
                                           XrProviderValuePack *result) {
    ByteHost *host = opaque;
    REQUIRE(arguments->count == 1u && arguments->nodes[0].token == XR_PROVIDER_TYPE_U8_ARRAY);
    size_t count = arguments->nodes[0].as.u8_array.size;
    REQUIRE(count == (XR_TYPED_PROVIDER_EMPTY ? 0u : 3u));
    for (size_t i = 0u; i < count; ++i) {
        uint8_t *byte = &arguments->nodes[0].as.u8_array.data[i];
        REQUIRE(*byte == (host->calls ? 40u + i : 7u));
        *byte = (uint8_t)(40u + i);
    }
    ++host->calls;
    result->count = 1u;
    result->nodes[0].token = host->mode == 2u ? XR_PROVIDER_TYPE_BOOL : XR_PROVIDER_TYPE_UNIT;
    return host->mode == 1u ? XR_PROVIDER_CALL_FAILED :
           host->mode == 3u ? XR_PROVIDER_CALL_OUT_OF_MEMORY : XR_PROVIDER_CALL_OK;
}

static XrInstance *byte_instance(XrValidatedProgram *program, XrTargetProfile *profile, ByteHost *host) {
    XrProviderOperationBinding operation = {.operation_id = {{1u}},
        .trampoline_kind = XR_PROVIDER_TRAMPOLINE_TYPED, .context = host, .entry.typed = byte_host_call};
    XrProviderBinding binding = {.contract_id = typed_contract_id,
        .behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL, .operations = &operation, .operation_count = 1u};
    for (size_t i = 0u; i < xr_target_profile_provider_count(profile); ++i) {
        const XrTargetProviderContract *p = xr_target_profile_provider(profile, i);
        if (memcmp(p->contract_id.bytes, typed_contract_id.bytes, XR_STABLE_ID_BYTES) == 0)
            REQUIRE(xr_target_provider_contract_fingerprint(p, &binding.contract_fingerprint) == XR_RUNTIME_ABI_OK);
    }
    XrExecutionBindingInput input = {.schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = program, .profile = profile, .providers = &binding, .provider_count = 1u, .generation = 1u};
    XrInstance *instance = NULL;
    REQUIRE(xr_execution_instance_create(&input, &instance, NULL) == XR_EXECUTION_OK);
    return instance;
}

#ifdef XR_NATIVE_RESOURCE_ALLOCATION_TEST
#include "../execution/xr_typed_provider_allocation_probe.h"

static void check_byte_native_allocations(XrValidatedProgram *program, XrTargetProfile *profile) {
    unsigned after_host = 0u;
    for (size_t failure = 1u; failure < 128u; ++failure) {
        ByteHost host = {0};
        XrInstance *instance = byte_instance(program, profile, &host);
        typed_attempt = 0u;
        typed_fail_at = failure;
        typed_failed = false;
        typed_armed = true;
        XrBackendExecution *execution = NULL;
        bool created = xr_backend_execution_create(instance, &xr_aot_entry_coroutine_descriptor, &execution);
        if (created) {
            XrBackendExecutionOutcome outcome = xr_backend_execution_step(execution);
            if (typed_failed) REQUIRE(outcome.kind == XR_BACKEND_EXECUTION_TRAP && outcome.safepoint_id == 4u);
            else REQUIRE(outcome.kind == XR_BACKEND_EXECUTION_RETURN && outcome.value == 42 && host.calls == 2u);
        } else REQUIRE(typed_failed && !execution);
        typed_armed = false;
        after_host += typed_failed && host.calls != 0u;
        xr_backend_execution_free(execution);
        REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
        REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
        REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);
        REQUIRE(typed_live_count == 0u);
        if (!typed_failed) {
            REQUIRE(typed_attempt + 1u == failure);
            REQUIRE(XR_TYPED_PROVIDER_EMPTY || after_host != 0u);
            printf("Native byte loan allocation points: %zu; after-host: %u\n", typed_attempt, after_host);
            return;
        }
    }
    abort();
}
#endif

int main(void) {
    XrValidatedProgram *program = typed_byte_program_build(XR_TYPED_PROVIDER_EMPTY != 0, 0u, true,
                                                          XR_TYPED_PROVIDER_PERSISTENT != 0);
    REQUIRE(typed_byte_program_build(false, 5u, true, true) == NULL);
    XrTargetProfile *profile = typed_profile_build(4u);
    REQUIRE(program && profile);
#ifdef XR_NATIVE_RESOURCE_ALLOCATION_TEST
    check_byte_native_allocations(program, profile);
#endif
    XrVmCode *code = NULL;
    REQUIRE(xr_vm_code_build(program, profile, NULL, &code, NULL) == XR_VM_CODE_OK);
    for (unsigned native = 0u; native < 2u; ++native) {
        for (unsigned mode = 0u; mode < 4u; ++mode) {
            ByteHost host = {.mode = mode};
            XrInstance *instance = byte_instance(program, profile, &host);
            if (native) {
                XrBackendExecution *execution = NULL;
                REQUIRE(xr_backend_execution_create(instance, &xr_aot_entry_coroutine_descriptor, &execution));
                XrBackendExecutionOutcome outcome = xr_backend_execution_step(execution);
                if (!mode) REQUIRE(outcome.kind == XR_BACKEND_EXECUTION_RETURN && outcome.value == 42);
                else REQUIRE(outcome.kind == XR_BACKEND_EXECUTION_TRAP && outcome.safepoint_id == (mode == 3u ? 4u : 7u));
                xr_backend_execution_free(execution);
            } else {
                XrVmOutcome outcome = xr_vm_code_execute(code, instance, 0u, NULL, 0u);
                if (!mode) REQUIRE(outcome.kind == XR_VM_OUTCOME_RETURN && outcome.value.as.i64 == 42);
                else if (mode == 3u) REQUIRE(outcome.kind == XR_VM_OUTCOME_RESOURCE_LIMIT);
                else REQUIRE(outcome.kind == XR_VM_OUTCOME_TRAP && outcome.trap == XR_VM_TRAP_PROVIDER_CALL_FAILED);
                xr_vm_outcome_dispose(&outcome);
            }
            REQUIRE(host.calls == (mode ? 1u : 2u));
            if (XR_TYPED_PROVIDER_PERSISTENT && mode) {
                host.mode = 0u;
                host.calls = 0u;
                /* Reenter the same module instance. The first callback checks
                 * every original byte before publishing a successful update. */
                if (native) {
                    XrBackendExecution *execution = NULL;
                    REQUIRE(xr_backend_execution_create(instance, &xr_aot_entry_coroutine_descriptor, &execution));
                    XrBackendExecutionOutcome outcome = xr_backend_execution_step(execution);
                    REQUIRE(outcome.kind == XR_BACKEND_EXECUTION_RETURN && outcome.value == 42);
                    xr_backend_execution_free(execution);
                } else {
                    XrVmOutcome outcome = xr_vm_code_execute(code, instance, 0u, NULL, 0u);
                    REQUIRE(outcome.kind == XR_VM_OUTCOME_RETURN && outcome.value.as.i64 == 42);
                    xr_vm_outcome_dispose(&outcome);
                }
                REQUIRE(host.calls == 2u);
            }
            REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
            REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
            REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);
        }
    }
    xr_vm_code_free(code);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
    return 0;
}
