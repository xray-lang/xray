/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_target_profile_authority.c - Production target authority wiring tests
 */

#include "../../../src/aot/xaot_driver.h"
#include "../../../src/app/toolchain/xtc_target_profile.h"
#include "../../../src/runtime/abi/xr_runtime_target_authority.h"
#include "../../../src/toolchain/xcompiler_session.h"
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,          \
                    #condition);                                                \
            failures++;                                                         \
        }                                                                       \
    } while (0)

static bool build_native_profile(XaotTarget *aot_target,
                                 XrTargetProfile **out) {
    XrTargetCodegenFacts codegen;
    char error[256];
    return xaot_target_profile_codegen_facts(aot_target, &codegen) &&
           xtc_target_profile_build_current_native_hosted(
               &codegen, out, error, sizeof(error));
}

static void test_native_authority_is_deterministic(void) {
    XaotTarget aot_target = {0};
    XrToolchainTarget toolchain_target;
    XrTargetProfile *first = NULL;
    XrTargetProfile *second = NULL;
    CHECK(xtc_target_parse("native", &toolchain_target, NULL, 0));
    CHECK(toolchain_target.is_native);
    CHECK(xaot_target_init(&aot_target, NULL));
    CHECK(build_native_profile(&aot_target, &first));
    CHECK(build_native_profile(&aot_target, &second));
    CHECK(xr_target_profile_require_exact(first, second, NULL, 0));
    const XrTargetMachineFacts *machine =
        xr_target_profile_machine_facts(first);
    CHECK(machine != NULL);
    CHECK(machine && machine->runtime_profile ==
                         XR_TARGET_RUNTIME_PROFILE_HOSTED);
    CHECK(machine && machine->data_layout.pointer.size == sizeof(void *));
    xr_target_profile_free(second);
    xr_target_profile_free(first);
    xaot_target_free(&aot_target);
}

static void test_runtime_owner_publishes_validated_structures(void) {
    XrRuntimeTargetAuthority authority;
    XrFingerprint first;
    XrFingerprint second;
    uint64_t provider_mask = 0;
    CHECK(xr_runtime_target_authority_native_hosted(&authority) ==
          XR_RUNTIME_ABI_OK);
    CHECK(authority.provider_count ==
          XR_RUNTIME_TARGET_AUTHORITY_PROVIDER_COUNT);
    CHECK(xr_runtime_abi_contract_fingerprint(&authority.runtime_abi, &first) ==
          XR_RUNTIME_ABI_OK);
    CHECK(xr_runtime_abi_contract_fingerprint(&authority.runtime_abi, &second) ==
          XR_RUNTIME_ABI_OK);
    CHECK(xr_fingerprint_equal(first, second));
    CHECK(xr_target_provider_set_fingerprint(
              authority.providers, authority.provider_count, &provider_mask,
              &second) == XR_RUNTIME_ABI_OK);
    CHECK(provider_mask ==
          (XR_TARGET_PROVIDER_MASK(XR_TARGET_PROVIDER_ALLOCATOR) |
           XR_TARGET_PROVIDER_MASK(XR_TARGET_PROVIDER_PANIC)));
    CHECK(xr_runtime_string_object_contract_verify(
              &authority.string_contract) == XR_RUNTIME_ABI_OK);
    CHECK(xr_runtime_string_literal_materialization_contract_verify(
              &authority.string_contract.literal_view) == XR_RUNTIME_ABI_OK);
    CHECK(authority.string_contract.literal_view.dynamic_tag ==
          XR_RUNTIME_STRING_LITERAL_DYNAMIC_TAG);
    authority.string_contract.literal_view.dynamic_tag++;
    CHECK(xr_runtime_string_object_contract_verify(
              &authority.string_contract) != XR_RUNTIME_ABI_OK);
}

static void test_cross_target_and_reserved_facts_fail_closed(void) {
    XaotTarget aot_target = {0};
    XrTargetCodegenFacts codegen;
    XrToolchainTarget native_target;
    XrToolchainTarget cross_target;
    XrTargetProfile *profile = (XrTargetProfile *) (uintptr_t) 1;
    char error[256];
    CHECK(xtc_target_parse("native", &native_target, NULL, 0));
    CHECK(xaot_target_init(&aot_target, NULL));
    CHECK(xaot_target_profile_codegen_facts(&aot_target, &codegen));
    codegen.reserved32 = 1;
    CHECK(!xtc_target_profile_build_native_hosted(
        &native_target, &codegen, &profile, error, sizeof(error)));
    CHECK(profile == NULL);
    codegen.reserved32 = 0;

    const char *const *names;
    size_t count = 0;
    names = xtc_target_supported_names(&count);
    bool found_cross = false;
    for (size_t i = 0; i < count; i++) {
        if (xtc_target_parse(names[i], &cross_target, NULL, 0) &&
            !cross_target.is_native) {
            found_cross = true;
            break;
        }
    }
    CHECK(found_cross);
    CHECK(found_cross && !xtc_target_profile_build_native_hosted(
                             &cross_target, &codegen, &profile, error,
                             sizeof(error)));
    CHECK(profile == NULL);
    xaot_target_free(&aot_target);
}

static void test_compiler_session_retains_exact_profile(void) {
    XaotTarget aot_target = {0};
    XrTargetProfile *profile = NULL;
    CHECK(xaot_target_init(&aot_target, NULL));
    CHECK(build_native_profile(&aot_target, &profile));
    XrCompilerSessionConfig config = {.target_profile = profile};
    XrCompilerSession *session = xr_compiler_session_new(&config);
    CHECK(session != NULL);
    xr_target_profile_free(profile);
    profile = NULL;
    const XrTargetProfile *installed =
        xr_compiler_session_target_profile(session);
    CHECK(installed != NULL);
    CHECK(installed && xr_target_profile_verify(installed, NULL, 0));
    CHECK(installed &&
          memcmp(xr_compiler_session_target_data_layout(session),
                 &xr_target_profile_machine_facts(installed)->data_layout,
                 sizeof(XrTargetDataLayout)) == 0);
    xr_compiler_session_delete(session);
    xaot_target_free(&aot_target);
}

static void test_compiler_session_rejects_conflicting_layout_authority(void) {
    XaotTarget aot_target = {0};
    XrTargetProfile *profile = NULL;
    XrTargetDataLayout conflicting_layout;
    CHECK(xaot_target_init(&aot_target, NULL));
    CHECK(build_native_profile(&aot_target, &profile));
    CHECK(sizeof(void *) == 8
              ? xr_target_data_layout_init_ilp32(&conflicting_layout)
              : xr_target_data_layout_init_lp64(&conflicting_layout));
    XrCompilerSessionConfig config = {
        .target_data_layout = &conflicting_layout,
        .target_profile = profile,
    };
    CHECK(xr_compiler_session_new(&config) == NULL);
    xr_target_profile_free(profile);
    xaot_target_free(&aot_target);
}

static void test_aot_rejects_missing_profile_before_source_work(void) {
    XaotTarget target = {0};
    XaotBuildOptions options = {0};
    XaotBuildResult result = {0};
    CHECK(xaot_target_init(&target, NULL));
    options.target = &target;
    options.profile = XAOT_BUILD_PROFILE_HOSTED;
    CHECK(xaot_build("definitely-missing.xr", &options, &result) != 0);
    CHECK(result.n_sources == 0);
    xaot_build_result_free(&result);
    xaot_target_free(&target);
}

int main(void) {
    test_native_authority_is_deterministic();
    test_runtime_owner_publishes_validated_structures();
    test_cross_target_and_reserved_facts_fail_closed();
    test_compiler_session_retains_exact_profile();
    test_compiler_session_rejects_conflicting_layout_authority();
    test_aot_rejects_missing_profile_before_source_work();
    if (failures) {
        fprintf(stderr, "%d target authority test(s) failed\n", failures);
        return 1;
    }
    puts("target profile authority tests passed");
    return 0;
}
