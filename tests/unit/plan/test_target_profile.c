/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_target_profile.c - Production TargetProfile authority tests
 */

#include "../../../src/plan/target/xr_target_profile_internal.h"
#include "target_profile_test_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

static XrTargetProfile *build_fixture(XrTestTargetProfileFixture *fixture) {
    XrTargetProfile *profile = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_profile_build(&fixture->input, &profile, error,
                                    sizeof(error)));
    REQUIRE(profile != NULL);
    return profile;
}

static void require_build_rejected(XrTestTargetProfileFixture *fixture) {
    XrTargetProfile *profile = (XrTargetProfile *) (uintptr_t) 1;
    char error[512] = {0};
    REQUIRE(!xr_target_profile_build(&fixture->input, &profile, error,
                                     sizeof(error)));
    REQUIRE(profile == NULL);
    REQUIRE(strncmp(error, "XR_TARGET_1000", strlen("XR_TARGET_1000")) == 0);
}

static void test_structured_build_kat_and_exact_gate(void) {
    static const uint8_t expected_object_header[XR_FINGERPRINT_BYTES] = {
        0xdb, 0xc2, 0x22, 0xe3, 0x1c, 0x79, 0x03, 0x6f,
        0xf4, 0x67, 0x98, 0x38, 0x21, 0xb5, 0x8c, 0x6e,
        0xe8, 0x99, 0xb4, 0x35, 0xef, 0x8c, 0xfa, 0xa4,
        0xb7, 0x62, 0x9b, 0xdd, 0x8b, 0x79, 0x4f, 0xf3,
    };
    static const uint8_t expected_provider_set[XR_FINGERPRINT_BYTES] = {
        0x31, 0x42, 0xfb, 0xdb, 0x71, 0x05, 0xd9, 0xda,
        0x10, 0x29, 0xf2, 0x5f, 0x02, 0xcb, 0xd4, 0xc7,
        0x1c, 0x79, 0xa3, 0xce, 0x2f, 0x0c, 0x9e, 0x87,
        0x0e, 0xba, 0xff, 0x90, 0xf7, 0x55, 0x81, 0x2f,
    };
    XrTestTargetProfileFixture first_fixture;
    XrTestTargetProfileFixture same_fixture;
    REQUIRE(xr_test_target_profile_fixture_init(
        &first_fixture, false, XR_TARGET_RUNTIME_PROFILE_HOSTED));
    REQUIRE(xr_test_target_profile_fixture_init(
        &same_fixture, false, XR_TARGET_RUNTIME_PROFILE_HOSTED));
    XrTargetProfile *first = build_fixture(&first_fixture);
    XrTargetProfile *same = build_fixture(&same_fixture);
    REQUIRE(first->facts.provider_mask ==
            (XR_TARGET_PROVIDER_MASK(XR_TARGET_PROVIDER_ALLOCATOR) |
             XR_TARGET_PROVIDER_MASK(XR_TARGET_PROVIDER_PANIC)));
    REQUIRE(memcmp(first->facts.object_header_fingerprint.bytes,
                   expected_object_header, sizeof(expected_object_header)) == 0);
    REQUIRE(memcmp(first->facts.provider_set_fingerprint.bytes,
                   expected_provider_set, sizeof(expected_provider_set)) == 0);
    REQUIRE(first->facts.schema_version == 2);
    REQUIRE(first->facts.string_literal.dynamic_tag ==
            XR_RUNTIME_STRING_LITERAL_DYNAMIC_TAG);
    REQUIRE(first->facts.string_literal.view_size ==
            sizeof(XrRuntimeStringLiteralView));
    REQUIRE(first->facts.string_literal.semantic_domain ==
            XR_STORAGE_CONST_SHARED);
    REQUIRE(first->facts.string_literal.backend_materialization ==
            XR_MATERIALIZE_STATIC_DATA);
    REQUIRE(xr_runtime_string_literal_materialization_contract_verify(
                &first->facts.string_literal) == XR_RUNTIME_ABI_OK);
    char error[512] = {0};
    REQUIRE(xr_target_profile_require_exact(first, same, error, sizeof(error)));
    REQUIRE(xr_fingerprint_equal(xr_target_profile_fingerprint(first),
                                 xr_target_profile_fingerprint(same)));
    xr_target_profile_free(same);
    xr_target_profile_free(first);
}

static void test_machine_provider_and_runtime_mutations_change_exact_identity(void) {
    XrTestTargetProfileFixture base_fixture;
    REQUIRE(xr_test_target_profile_fixture_init(
        &base_fixture, false, XR_TARGET_RUNTIME_PROFILE_HOSTED));
    XrTargetProfile *base = build_fixture(&base_fixture);
    char error[512] = {0};

    XrTestTargetProfileFixture field_fixture;
    REQUIRE(xr_test_target_profile_fixture_init(
        &field_fixture, false, XR_TARGET_RUNTIME_PROFILE_HOSTED));
    field_fixture.input.machine.atomic_width_mask |= XR_TARGET_ATOMIC_WIDTH_128;
    XrTargetProfile *field_changed = build_fixture(&field_fixture);
    REQUIRE(!xr_target_profile_require_exact(base, field_changed, error,
                                             sizeof(error)));

    XrTestTargetProfileFixture provider_fixture;
    REQUIRE(xr_test_target_profile_fixture_init(
        &provider_fixture, false, XR_TARGET_RUNTIME_PROFILE_HOSTED));
    provider_fixture.providers[0].allocator_max_alignment = 128;
    XrTargetProfile *provider_changed = build_fixture(&provider_fixture);
    REQUIRE(!xr_target_profile_require_exact(base, provider_changed, error,
                                             sizeof(error)));

    XrTestTargetProfileFixture runtime_fixture;
    REQUIRE(xr_test_target_profile_fixture_init(
        &runtime_fixture, false, XR_TARGET_RUNTIME_PROFILE_HOSTED));
    runtime_fixture.runtime_abi.dynamic_value.tags[1].required_flags = 2;
    XrTargetProfile *runtime_changed = build_fixture(&runtime_fixture);
    REQUIRE(!xr_target_profile_require_exact(base, runtime_changed, error,
                                             sizeof(error)));

    xr_target_profile_free(runtime_changed);
    xr_target_profile_free(provider_changed);
    xr_target_profile_free(field_changed);
    xr_target_profile_free(base);
}

static void test_object_and_provider_mismatches_fail_atomically(void) {
    XrTestTargetProfileFixture fixture;
    REQUIRE(xr_test_target_profile_fixture_init(
        &fixture, false, XR_TARGET_RUNTIME_PROFILE_HOSTED));
    fixture.object_header_materialization.target_endian = XR_RUNTIME_ENDIAN_BIG;
    require_build_rejected(&fixture);

    REQUIRE(xr_test_target_profile_fixture_init(
        &fixture, false, XR_TARGET_RUNTIME_PROFILE_HOSTED));
    fixture.providers[1].runtime_profile =
        XR_TARGET_RUNTIME_PROFILE_FREESTANDING;
    require_build_rejected(&fixture);

    REQUIRE(xr_test_target_profile_fixture_init(
        &fixture, false, XR_TARGET_RUNTIME_PROFILE_HOSTED));
    fixture.input.provider_count = 0;
    require_build_rejected(&fixture);

    REQUIRE(xr_test_target_profile_fixture_init(
        &fixture, false, XR_TARGET_RUNTIME_PROFILE_HOSTED));
    fixture.input.provider_count = XR_RUNTIME_ABI_MAX_PROVIDERS + 1u;
    require_build_rejected(&fixture);

    REQUIRE(xr_test_target_profile_fixture_init(
        &fixture, false, XR_TARGET_RUNTIME_PROFILE_HOSTED));
    fixture.input.string_contract = NULL;
    require_build_rejected(&fixture);

    REQUIRE(xr_test_target_profile_fixture_init(
        &fixture, false, XR_TARGET_RUNTIME_PROFILE_HOSTED));
    fixture.string_contract.literal_view.dynamic_tag++;
    require_build_rejected(&fixture);

    REQUIRE(xr_test_target_profile_fixture_init(
        &fixture, false, XR_TARGET_RUNTIME_PROFILE_HOSTED));
    fixture.string_contract.literal_view.fields[4].offset++;
    require_build_rejected(&fixture);

    REQUIRE(xr_test_target_profile_fixture_init(
        &fixture, false, XR_TARGET_RUNTIME_PROFILE_HOSTED));
    fixture.string_contract.literal_view.fingerprint.bytes[3] ^= 1;
    require_build_rejected(&fixture);
}

static void test_runtime_profile_is_independent_of_os_environment(void) {
    XrTestTargetProfileFixture fixture;
    REQUIRE(xr_test_target_profile_fixture_init(
        &fixture, false, XR_TARGET_RUNTIME_PROFILE_FREESTANDING));
    REQUIRE(fixture.input.machine.operating_system == XR_TARGET_OS_WINDOWS);
    REQUIRE(fixture.input.machine.environment == XR_TARGET_ENV_MSVC);
    XrTargetProfile *profile = build_fixture(&fixture);
    REQUIRE(xr_target_profile_verify(profile, NULL, 0));
    xr_target_profile_free(profile);
}

int main(void) {
    test_structured_build_kat_and_exact_gate();
    test_machine_provider_and_runtime_mutations_change_exact_identity();
    test_object_and_provider_mismatches_fail_atomically();
    test_runtime_profile_is_independent_of_os_environment();
    puts("production TargetProfile tests passed");
    return 0;
}
