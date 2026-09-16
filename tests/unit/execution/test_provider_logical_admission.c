/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_provider_logical_admission.c - Exact Program and provider semantics
 */

#include "aot/program/xr_backend_ir.h"
#include "execution/xr_execution.h"
#include "execution/xr_execution_identity.h"
#include "../plan/target_profile_test_fixture.h"
#include "../program/xr_program_trap_fixture.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

static XrProviderCallStatus clock_read(void *context, int64_t *out) {
    (void) context;
    *out = 42;
    return XR_PROVIDER_CALL_OK;
}

static const XrTargetProviderContract *ordinary_provider(const XrTargetProfile *profile) {
    for (size_t index = 0u; index < xr_target_profile_provider_count(profile); ++index) {
        const XrTargetProviderContract *provider = xr_target_profile_provider(profile, index);
        if (provider && provider->provider_role == XR_TARGET_PROVIDER_ROLE_OPERATIONS)
            return provider;
    }
    return NULL;
}

static XrTargetProfile *changed_profile(const XrTargetProfile *base,
                                        const XrProviderLogicalContract *logical) {
    XrTestTargetProfileFixture fixture;
    CHECK(xr_test_target_profile_fixture_init(&fixture, false, XR_TARGET_RUNTIME_PROFILE_HOSTED));
    XrTargetProviderContract providers[3];
    CHECK(xr_target_profile_provider_count(base) == 3u);
    for (size_t index = 0u; index < 3u; ++index) {
        providers[index] = *xr_target_profile_provider(base, index);
        if (providers[index].provider_role == XR_TARGET_PROVIDER_ROLE_OPERATIONS)
            providers[index].operations[0].logical_contract = *logical;
    }
    fixture.input.providers = providers;
    fixture.input.provider_count = 3u;
    XrTargetProfile *result = NULL;
    char error[256] = {0};
    if (!xr_target_profile_build(&fixture.input, &result, error, sizeof(error)))
        fprintf(stderr, "profile rejected: %s\n", error);
    return result;
}

static void check_admission(XrValidatedProgram *program, XrTargetProfile *profile, bool admitted) {
    const XrTargetProviderContract *provider = ordinary_provider(profile);
    CHECK(provider && provider->operation_count == 1u);
    XrProviderOperationBinding operation = {
        .operation_id = provider->operations[0].stable_id,
        .trampoline_kind = XR_PROVIDER_TRAMPOLINE_I64_NULLARY,
        .entry.i64_nullary = clock_read,
    };
    XrProviderBinding binding = {
        .contract_id = provider->contract_id,
        .behavior_flags = XR_PROVIDER_BEHAVIOR_THREAD_SAFE | XR_PROVIDER_BEHAVIOR_REENTRANT |
                          XR_PROVIDER_BEHAVIOR_CALLBACK_SAFE,
        .operations = &operation,
        .operation_count = 1u,
    };
    CHECK(xr_target_provider_contract_fingerprint(provider, &binding.contract_fingerprint) ==
          XR_RUNTIME_ABI_OK);
    XrExecutionBindingInput input = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = program,
        .profile = profile,
        .providers = &binding,
        .provider_count = 1u,
        .generation = 1u,
    };
    XrInstance *instance = NULL;
    XrExecutionDiagnostic diagnostic;
    XrExecutionStatus status = xr_execution_instance_create(&input, &instance, &diagnostic);
    CHECK(status == (admitted ? XR_EXECUTION_OK : XR_EXECUTION_PROFILE_REJECTED));
    if (admitted) {
        CHECK(instance != NULL);
        CHECK(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
        CHECK(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
        CHECK(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);
    } else {
        CHECK(instance == NULL && diagnostic.kind == XR_EXECUTION_DIAGNOSTIC_PROFILE);
    }
    XrExecutionId identity;
    CHECK(xr_execution_id_compute(program, profile, &identity) == admitted);
    XrBackendOptions options = xr_backend_default_options();
    XrBackendIR *backend = NULL;
    XrBackendStatus aot_status = xr_backend_ir_build(program, profile, &options, &backend, NULL);
    CHECK(aot_status == (admitted ? XR_BACKEND_OK : XR_BACKEND_INVALID_INPUT));
    CHECK((backend != NULL) == admitted);
    xr_backend_ir_free(backend);
}

int main(void) {
    XrTargetProfile *base = xr_test_target_profile_build_with_nullary_clock(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED, XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER);
    CHECK(base != NULL);
    CHECK(xr_target_profile_machine_facts(base)->operating_system == XR_TARGET_OS_WINDOWS);
    const XrTargetProviderContract *provider = ordinary_provider(base);
    CHECK(provider != NULL);
    XrProgramArtifact artifact = {0};
    char error[256] = {0};
    CHECK(xr_program_trap_fixture_write_with_ids(provider->contract_id,
                                                 provider->operations[0].stable_id, &artifact,
                                                 error, sizeof(error)) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    CHECK(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
          XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    check_admission(program, base, true);
    for (unsigned mutation = 0u; mutation < 6u; ++mutation) {
        XrProviderLogicalContract logical = provider->operations[0].logical_contract;
        switch (mutation) {
            case 0:
                logical.effects = XR_PROVIDER_EFFECT_READS_PROCESS;
                break;
            case 1:
                logical.threads = XR_PROVIDER_THREADS_INSTANCE_AFFINE;
                break;
            case 2:
                logical.reentry = XR_PROVIDER_REENTRY_FORBIDDEN;
                break;
            case 3:
                logical.callbacks = XR_PROVIDER_CALLBACK_SYNCHRONOUS;
                break;
            case 4:
                logical.result_owner = XR_PROVIDER_OWNER_BORROWED;
                break;
            case 5:
                logical.platforms = XR_PROVIDER_PLATFORM_WINDOWS;
                break;
        }
        XrTargetProfile *changed = changed_profile(base, &logical);
        CHECK(changed != NULL);
        CHECK(!xr_fingerprint_equal(xr_target_profile_fingerprint(base),
                                    xr_target_profile_fingerprint(changed)));
        check_admission(program, changed, false);
        xr_target_profile_free(changed);
    }
    XrProviderLogicalContract unavailable = provider->operations[0].logical_contract;
    unavailable.platforms = XR_PROVIDER_PLATFORM_LINUX;
    CHECK(changed_profile(base, &unavailable) == NULL);
    unavailable = provider->operations[0].logical_contract;
    unavailable.runtime_profiles = XR_PROVIDER_LOGICAL_PROFILE_FREESTANDING;
    CHECK(changed_profile(base, &unavailable) == NULL);
    xr_validated_program_free(program);
    xr_target_profile_free(base);
    puts("provider logical admission tests passed");
    return 0;
}
