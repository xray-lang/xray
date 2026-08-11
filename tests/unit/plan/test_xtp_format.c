/*
 * test_xtp_format.c - Exact typed TargetPlan artifact contract
 */

#include "../../../src/ir/xi.h"
#include "../../../src/plan/format/xr_xtp_internal.h"
#include "../../../src/plan/format/xr_artifact_kind.h"
#include "../../../src/plan/semantic/xr_semantic_builder.h"
#include "../../../src/plan/target/xr_target_builder.h"
#include "../../../src/runtime/abi/xr_runtime_target_authority.h"
#include "../../../include/xray_target_plan_load.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/base/xsha256.h"
#include "../../../src/runtime/value/xtype.h"
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

typedef struct XtpFixture {
    XrSemanticPlan *semantic;
    XrTargetProfile *profile;
    XrTargetPlan *plan;
    uint8_t *bytes;
    size_t size;
} XtpFixture;

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};

static XrSemanticPlan *build_semantic_plan(void) {
    XiFunc *function = xi_func_new("xtp_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    REQUIRE(xi_const_int(function, entry, 7, &stub_int) != NULL);
    XiValue *result = xi_const_int(function, entry, 42, &stub_int);
    REQUIRE(result != NULL);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(function, &semantic, error, sizeof(error)));
    REQUIRE(semantic != NULL);
    xi_func_free(function);
    return semantic;
}

static XrTargetProfile *build_profile(void) {
    XrTargetProfile *selection = xr_test_target_profile_build(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(selection != NULL);
    XrRuntimeTargetAuthority authority;
    REQUIRE(xr_runtime_target_authority_native_hosted(&authority) ==
            XR_RUNTIME_ABI_OK);
    XrTargetProfileBuildInput input = {
        .machine = *xr_target_profile_machine_facts(selection),
        .runtime_abi = &authority.runtime_abi,
        .object_header_materialization =
            &authority.object_header_materialization,
        .providers = authority.providers,
        .provider_count = authority.provider_count,
    };
    XrTargetProfile *profile = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_profile_build(&input, &profile, error, sizeof(error)));
    xr_target_profile_free(selection);
    return profile;
}

static XtpFixture make_fixture(void) {
    XtpFixture fixture = {0};
    fixture.semantic = build_semantic_plan();
    fixture.profile = build_profile();
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(fixture.semantic, fixture.profile, &fixture.plan, error,
                                 sizeof(error)));
    REQUIRE(xr_target_plan_is_verified(fixture.plan));
    REQUIRE(xr_xtp_encode_plan(fixture.plan, &fixture.bytes, &fixture.size, error,
                               sizeof(error)));
    REQUIRE(fixture.bytes != NULL && fixture.size >= XR_XTP_HEADER_SIZE);
    return fixture;
}

static void dispose_fixture(XtpFixture *fixture) {
    xr_xtp_encoded_free(fixture->bytes);
    xr_target_plan_free(fixture->plan);
    xr_target_profile_free(fixture->profile);
    xr_semantic_plan_free(fixture->semantic);
    memset(fixture, 0, sizeof(*fixture));
}

static uint8_t *copy_artifact(const XtpFixture *fixture) {
    uint8_t *copy = (uint8_t *) xr_malloc(fixture->size);
    REQUIRE(copy != NULL);
    memcpy(copy, fixture->bytes, fixture->size);
    return copy;
}

static void resign_artifact(uint8_t *bytes, size_t size) {
    static const uint8_t zero[XR_FINGERPRINT_BYTES] = {0};
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, bytes, XR_XTP_FULL_DIGEST_OFFSET);
    xr_sha256_update(&context, zero, sizeof(zero));
    xr_sha256_update(&context, bytes + XR_XTP_FULL_DIGEST_OFFSET + sizeof(zero),
                     size - XR_XTP_FULL_DIGEST_OFFSET - sizeof(zero));
    xr_sha256_final(&context, bytes + XR_XTP_FULL_DIGEST_OFFSET);
}

static uint8_t *directory_entry(uint8_t *bytes, XrXtpSectionKind kind) {
    return bytes + XR_XTP_HEADER_SIZE +
           ((size_t) kind - 1u) * XR_XTP_DIRECTORY_ENTRY_SIZE;
}

static void resign_section(uint8_t *bytes, XrXtpSectionKind kind) {
    uint8_t *entry = directory_entry(bytes, kind);
    size_t offset = (size_t) xr_xtp_take_u64(entry + 8);
    size_t length = (size_t) xr_xtp_take_u64(entry + 16);
    xr_sha256(bytes + offset, length, entry + 40);
}

static void expect_decode_failure(const uint8_t *bytes, size_t size) {
    XrXtpCandidate *candidate = (XrXtpCandidate *) (uintptr_t) 1;
    char error[512] = {0};
    REQUIRE(!xr_xtp_decode_candidate(bytes, size, &candidate, error, sizeof(error)));
    REQUIRE(candidate == NULL);
    REQUIRE(error[0] != '\0');
}

static void expect_materialize_failure(const XtpFixture *fixture, const uint8_t *bytes) {
    XrXtpCandidate *candidate = NULL;
    char error[512] = {0};
    REQUIRE(xr_xtp_decode_candidate(bytes, fixture->size, &candidate, error, sizeof(error)));
    XrTargetPlan *plan = (XrTargetPlan *) (uintptr_t) 1;
    REQUIRE(!xr_xtp_materialize_target_plan(candidate, fixture->semantic, fixture->profile,
                                             &plan, error, sizeof(error)));
    REQUIRE(plan == NULL);
    xr_xtp_candidate_release(candidate);
}

static void test_exact_roundtrip_and_owned_candidate(void) {
    XtpFixture fixture = make_fixture();
    XrXtpCandidate *candidate = NULL;
    char error[512] = {0};
    REQUIRE(xr_xtp_decode_candidate(fixture.bytes, fixture.size, &candidate, error,
                                    sizeof(error)));
    REQUIRE(xr_xtp_candidate_retain(candidate) == candidate);
    xr_xtp_candidate_release(candidate);
    XrXtpIdentity identity;
    XrXtpResourceManifest resources;
    REQUIRE(xr_xtp_candidate_identity(candidate, &identity));
    REQUIRE(xr_xtp_candidate_resources(candidate, &resources));
    REQUIRE(identity.plan_schema == XR_TARGET_PLAN_SCHEMA_VERSION);
    REQUIRE(identity.completed_family_mask == XR_TARGET_REQUIRED_FAMILIES);
    REQUIRE(resources.total_rows > 1 && resources.verification_work_units == resources.total_rows);

    uint8_t saved = fixture.bytes[0];
    fixture.bytes[0] ^= 0xff;
    XrTargetPlan *decoded_plan = NULL;
    REQUIRE(xr_xtp_materialize_target_plan(candidate, fixture.semantic, fixture.profile,
                                            &decoded_plan, error, sizeof(error)));
    REQUIRE(xr_target_plan_is_verified(decoded_plan));
    REQUIRE(xr_fingerprint_equal(xr_target_plan_fingerprint(decoded_plan),
                                 xr_target_plan_fingerprint(fixture.plan)));
    fixture.bytes[0] = saved;

    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    REQUIRE(xr_xtp_encode_plan(decoded_plan, &encoded, &encoded_size, error, sizeof(error)));
    REQUIRE(encoded_size == fixture.size);
    REQUIRE(memcmp(encoded, fixture.bytes, fixture.size) == 0);
    xr_xtp_encoded_free(encoded);
    xr_target_plan_free(decoded_plan);
    xr_xtp_candidate_release(candidate);
    dispose_fixture(&fixture);
}

static void test_artifact_classifier(void) {
    static const uint8_t source[] = "print(1)";
    static const uint8_t removed_xtp_v1[] = {
        'X', 'R', 'A', 'Y', 'X', 'T', 'P', 0};
    REQUIRE(xr_artifact_classify("renamed.bin", xr_xsm_artifact_magic,
                                 XR_XSM_ARTIFACT_MAGIC_SIZE) ==
            XR_ARTIFACT_KIND_XSM);
    REQUIRE(xr_artifact_classify("renamed.bin", xr_xtp_artifact_magic,
                                 XR_XTP_ARTIFACT_MAGIC_SIZE) ==
            XR_ARTIFACT_KIND_XTP);
    REQUIRE(xr_artifact_classify("renamed.bin", xr_legacy_xrc_artifact_magic,
                                 XR_LEGACY_XRC_ARTIFACT_MAGIC_SIZE) ==
            XR_ARTIFACT_KIND_LEGACY_XRC);
    REQUIRE(xr_artifact_classify("program.xr", source, sizeof(source) - 1) ==
            XR_ARTIFACT_KIND_SOURCE);
    REQUIRE(xr_artifact_classify("program.xtp", source, sizeof(source) - 1) ==
            XR_ARTIFACT_KIND_CONFLICT);
    REQUIRE(xr_artifact_classify("program.xsm", xr_xtp_artifact_magic,
                                 XR_XTP_ARTIFACT_MAGIC_SIZE) ==
            XR_ARTIFACT_KIND_CONFLICT);
    REQUIRE(xr_artifact_classify("renamed.bin", removed_xtp_v1,
                                 sizeof(removed_xtp_v1)) ==
            XR_ARTIFACT_KIND_UNSUPPORTED);
}

static void test_runtime_load_materializes_only_verified_plan(void) {
    XtpFixture fixture = make_fixture();
    char diagnostic[512] = {0};
    XrTargetPlan *loaded = NULL;
    REQUIRE(xr_runtime_target_plan_load(
        fixture.bytes, fixture.size, fixture.semantic, fixture.profile,
        &loaded, diagnostic, sizeof(diagnostic)));
    REQUIRE(loaded != NULL && xr_target_plan_is_verified(loaded));
    REQUIRE(xr_fingerprint_equal(xr_target_plan_fingerprint(loaded),
                                 xr_target_plan_fingerprint(fixture.plan)));
    xr_target_plan_free(loaded);

    loaded = (XrTargetPlan *) (uintptr_t) 1;
    REQUIRE(!xr_runtime_target_plan_load(
        fixture.bytes, fixture.size, NULL, fixture.profile, &loaded,
        diagnostic, sizeof(diagnostic)));
    REQUIRE(loaded == NULL);
    REQUIRE(strstr(diagnostic, "XR_ARTIFACT_2004") != NULL);

    static const uint8_t xsm[] = {'X', 'R', 'A', 'Y', 'X', 'S', 'M', 0};
    loaded = (XrTargetPlan *) (uintptr_t) 1;
    REQUIRE(!xr_runtime_target_plan_load(
        xsm, sizeof(xsm), fixture.semantic, fixture.profile, &loaded,
        diagnostic, sizeof(diagnostic)));
    REQUIRE(loaded == NULL);
    REQUIRE(strstr(diagnostic, "XR_ARTIFACT_2000") != NULL);
    dispose_fixture(&fixture);
}

static void test_wire_row_inventory(void) {
    static const uint32_t expected[] = {
        0, 292, 58, 12, 24, 108, 24, 40, 24, 12,
        40, 58, 62, 8, 20, 4, 20, 20, 12, 40,
    };
    REQUIRE(sizeof(expected) / sizeof(expected[0]) == XR_XTP_SECTION_COUNT);
    for (uint32_t kind = 1; kind < XR_XTP_SECTION_COUNT; kind++) {
        REQUIRE(xr_xtp_wire_row_size((XrXtpSectionKind) kind) == expected[kind]);
        REQUIRE(xr_xtp_table_count_limit((XrXtpSectionKind) kind) > 0);
    }
}

static void require_row_codec_roundtrip(XrXtpSectionKind kind, size_t native_size) {
    uint32_t wire_size = xr_xtp_wire_row_size(kind);
    REQUIRE(wire_size > 0 && wire_size <= 512);
    void *source = xr_malloc(native_size);
    void *decoded = xr_calloc(1, native_size);
    REQUIRE(source != NULL && decoded != NULL);
    memset(source, 0xa5, native_size);
    uint8_t first[512] = {0};
    uint8_t second[512] = {0};
    REQUIRE(xr_xtp_encode_rows(kind, source, 1, first));
    REQUIRE(xr_xtp_decode_rows(kind, first, 1, decoded));
    REQUIRE(xr_xtp_encode_rows(kind, decoded, 1, second));
    REQUIRE(memcmp(first, second, wire_size) == 0);
    xr_free(decoded);
    xr_free(source);
}

static void test_every_typed_row_codec(void) {
#define XR_XTP_ROW_ROUNDTRIP(kind, type)                                                           \
    require_row_codec_roundtrip(XR_XTP_SECTION_##kind, sizeof(type))
    XR_XTP_ROW_ROUNDTRIP(TARGET_PROFILE, XrTargetProfileDraft);
    XR_XTP_ROW_ROUNDTRIP(MACHINE_REPS, XrTargetMachineRepRecord);
    XR_XTP_ROW_ROUNDTRIP(VALUE_REPS, XrTargetValueRepRecord);
    XR_XTP_ROW_ROUNDTRIP(EXTENTS, XrTargetExtentRecord);
    XR_XTP_ROW_ROUNDTRIP(LAYOUTS, XrTargetLayoutRecord);
    XR_XTP_ROW_ROUNDTRIP(FIELDS, XrTargetFieldRecord);
    XR_XTP_ROW_ROUNDTRIP(STORAGE, XrTargetStorageRecord);
    XR_XTP_ROW_ROUNDTRIP(ALLOCATIONS, XrTargetAllocationRecord);
    XR_XTP_ROW_ROUNDTRIP(EXTENT_OPERANDS, XrTargetExtentOperandRecord);
    XR_XTP_ROW_ROUNDTRIP(FUNCTIONS, XrTargetFunctionRecord);
    XR_XTP_ROW_ROUNDTRIP(SLOTS, XrTargetSlotRecord);
    XR_XTP_ROW_ROUNDTRIP(CALLS, XrTargetCallRecord);
    XR_XTP_ROW_ROUNDTRIP(CALL_ARGUMENTS, XrTargetCallArgumentRecord);
    XR_XTP_ROW_ROUNDTRIP(ROOT_MAPS, XrTargetRootMapRecord);
    XR_XTP_ROW_ROUNDTRIP(ROOT_SLOTS, uint32_t);
    XR_XTP_ROW_ROUNDTRIP(CLEANUPS, XrTargetCleanupRecord);
    XR_XTP_ROW_ROUNDTRIP(ADAPTERS, XrTargetAdapterRecord);
    XR_XTP_ROW_ROUNDTRIP(CAPABILITIES, XrTargetCapabilityRecord);
    XR_XTP_ROW_ROUNDTRIP(COROUTINES, XrTargetCoroutineStateRecord);
#undef XR_XTP_ROW_ROUNDTRIP
}

static void test_header_and_directory_mutations(void) {
    XtpFixture fixture = make_fixture();
    uint8_t *copy = copy_artifact(&fixture);
    copy[XR_XTP_FULL_DIGEST_OFFSET] ^= 1;
    expect_decode_failure(copy, fixture.size);

    memcpy(copy, fixture.bytes, fixture.size);
    xr_xtp_put_u32(copy + 4, 1);
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);

    memcpy(copy, fixture.bytes, fixture.size);
    xr_xtp_put_u64(copy + 48, 0);
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);

    memcpy(copy, fixture.bytes, fixture.size);
    xr_xtp_put_u64(copy + 296, XR_XTP_MAX_TOTAL_ROWS + 1u);
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);
    expect_decode_failure(fixture.bytes, XR_XTP_MAX_ARTIFACT_SIZE + 1u);

    uint8_t *entry = directory_entry(copy, XR_XTP_SECTION_FUNCTIONS);
    memcpy(copy, fixture.bytes, fixture.size);
    entry = directory_entry(copy, XR_XTP_SECTION_FUNCTIONS);
    xr_xtp_put_u64(entry + 8, UINT64_MAX);
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);

    memcpy(copy, fixture.bytes, fixture.size);
    entry = directory_entry(copy, XR_XTP_SECTION_FUNCTIONS);
    xr_xtp_put_u64(entry + 16, xr_xtp_take_u64(entry + 16) + 1u);
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);

    memcpy(copy, fixture.bytes, fixture.size);
    entry = directory_entry(copy, XR_XTP_SECTION_FUNCTIONS);
    xr_xtp_put_u64(entry + 24, xr_xtp_take_u64(entry + 24) + 1u);
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);

    memcpy(copy, fixture.bytes, fixture.size);
    entry = directory_entry(copy, XR_XTP_SECTION_FUNCTIONS);
    xr_xtp_put_u32(entry + 32, xr_xtp_take_u32(entry + 32) + 1u);
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);

    memcpy(copy, fixture.bytes, fixture.size);
    entry = directory_entry(copy, XR_XTP_SECTION_FUNCTIONS);
    xr_xtp_put_u32(entry + 36, 8);
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);

    memcpy(copy, fixture.bytes, fixture.size);
    xr_xtp_put_u32(directory_entry(copy, XR_XTP_SECTION_TARGET_PROFILE),
                   XR_XTP_SECTION_MACHINE_REPS);
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);

    memcpy(copy, fixture.bytes, fixture.size);
    entry = directory_entry(copy, XR_XTP_SECTION_FUNCTIONS);
    entry[40] ^= 1;
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);
    xr_free(copy);
    dispose_fixture(&fixture);
}

static void test_identity_and_typed_mutations(void) {
    XtpFixture fixture = make_fixture();
    static const size_t identity_offsets[] = {72, 104, 136, 168, 200, 232, 264};
    for (size_t i = 0; i < sizeof(identity_offsets) / sizeof(identity_offsets[0]); i++) {
        uint8_t *copy = copy_artifact(&fixture);
        copy[identity_offsets[i]] ^= 1;
        resign_artifact(copy, fixture.size);
        expect_materialize_failure(&fixture, copy);
        xr_free(copy);
    }

    uint8_t *copy = copy_artifact(&fixture);
    uint8_t *function_entry = directory_entry(copy, XR_XTP_SECTION_FUNCTIONS);
    size_t function_offset = (size_t) xr_xtp_take_u64(function_entry + 8);
    copy[function_offset + 16] ^= 1;
    resign_section(copy, XR_XTP_SECTION_FUNCTIONS);
    resign_artifact(copy, fixture.size);
    expect_materialize_failure(&fixture, copy);
    xr_free(copy);

    copy = copy_artifact(&fixture);
    function_entry = directory_entry(copy, XR_XTP_SECTION_FUNCTIONS);
    function_offset = (size_t) xr_xtp_take_u64(function_entry + 8);
    copy[function_offset + 20] ^= 1;
    resign_section(copy, XR_XTP_SECTION_FUNCTIONS);
    resign_artifact(copy, fixture.size);
    expect_materialize_failure(&fixture, copy);
    xr_free(copy);

    static const size_t slot_mutations[] = {0, 24, 28, 32, 50};
    for (size_t i = 0; i < sizeof(slot_mutations) / sizeof(slot_mutations[0]); i++) {
        copy = copy_artifact(&fixture);
        uint8_t *slot_entry = directory_entry(copy, XR_XTP_SECTION_SLOTS);
        size_t slot_offset = (size_t) xr_xtp_take_u64(slot_entry + 8);
        copy[slot_offset + slot_mutations[i]] ^= 1;
        resign_section(copy, XR_XTP_SECTION_SLOTS);
        resign_artifact(copy, fixture.size);
        expect_materialize_failure(&fixture, copy);
        xr_free(copy);
    }

    copy = copy_artifact(&fixture);
    uint8_t *slot_entry = directory_entry(copy, XR_XTP_SECTION_SLOTS);
    size_t slot_offset = (size_t) xr_xtp_take_u64(slot_entry + 8);
    uint32_t slot_count = (uint32_t) xr_xtp_take_u64(slot_entry + 24);
    REQUIRE(slot_count >= 2);
    uint32_t slot_size = xr_xtp_take_u32(slot_entry + 32);
    memcpy(copy + slot_offset + slot_size, copy + slot_offset, XR_STABLE_ID_BYTES);
    resign_section(copy, XR_XTP_SECTION_SLOTS);
    resign_artifact(copy, fixture.size);
    expect_materialize_failure(&fixture, copy);
    xr_free(copy);

    copy = copy_artifact(&fixture);
    xr_xtp_put_u64(copy + 312, xr_xtp_take_u64(copy + 312) + 1u);
    resign_artifact(copy, fixture.size);
    expect_materialize_failure(&fixture, copy);
    xr_free(copy);
    dispose_fixture(&fixture);
}

static int write_fixture(const char *path) {
    XtpFixture fixture = make_fixture();
    FILE *file = fopen(path, "wb");
    if (!file) {
        dispose_fixture(&fixture);
        return 1;
    }
    bool written = fwrite(fixture.bytes, 1, fixture.size, file) == fixture.size;
    bool closed = fclose(file) == 0;
    dispose_fixture(&fixture);
    return written && closed ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--write") == 0)
        return write_fixture(argv[2]);
    test_artifact_classifier();
    test_wire_row_inventory();
    test_every_typed_row_codec();
    test_exact_roundtrip_and_owned_candidate();
    test_header_and_directory_mutations();
    test_identity_and_typed_mutations();
    test_runtime_load_materializes_only_verified_plan();
    puts("typed XTP format tests passed");
    return 0;
}
