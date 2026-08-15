/*
 * fuzz_xtp_decode.c - Typed artifact decoder and verifier fuzz entry
 */

#include "../../src/ir/xi.h"
#include "../../src/plan/format/xr_xtp_internal.h"
#include "../../src/plan/semantic/xr_semantic_builder.h"
#include "../../src/plan/target/xr_target_builder.h"
#include "../../src/base/xmalloc.h"
#include "../../src/base/xsha256.h"
#include "../../src/runtime/xr_runtime_artifact_authority_internal.h"
#include "../../src/runtime/value/xtype.h"
#include "../unit/plan/target_profile_test_fixture.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct XtpFuzzFixture {
    XrSemanticPlan *semantic;
    XrTargetProfile *profile;
    XrTargetPlan *plan;
    XrRuntimeArtifactAuthority *authority;
    uint8_t *bytes;
    size_t size;
    bool ready;
} XtpFuzzFixture;

typedef enum XtpMutationExpectation {
    XTP_EXPECT_ANY = 0,
    XTP_EXPECT_VALID,
    XTP_EXPECT_DECODE_REJECTION,
    XTP_EXPECT_MATERIALIZE_REJECTION,
} XtpMutationExpectation;

typedef enum XtpMutation {
    XTP_MUTATION_VALID = 0,
    XTP_MUTATION_SCHEMA_VERSION,
    XTP_MUTATION_FULL_DIGEST,
    XTP_MUTATION_SEMANTIC_IDENTITY,
    XTP_MUTATION_PLAN_FINGERPRINT,
    XTP_MUTATION_SECTION_BOUNDS,
    XTP_MUTATION_SECTION_LENGTH,
    XTP_MUTATION_SECTION_COUNT,
    XTP_MUTATION_SECTION_ORDER,
    XTP_MUTATION_SECTION_DIGEST,
    XTP_MUTATION_UNKNOWN_OPCODE,
    XTP_MUTATION_UNKNOWN_RUNTIME_TAG,
    XTP_MUTATION_UNKNOWN_CONSTANT_FORM,
    XTP_MUTATION_TOTAL_ROWS_BUDGET,
    XTP_MUTATION_VERIFY_WORK_BUDGET,
    XTP_MUTATION_COUNT,
} XtpMutation;

typedef struct XtpMutationCase {
    uint8_t seed_code;
    XtpMutation mutation;
    XtpMutationExpectation expectation;
    const char *name;
} XtpMutationCase;

static const XtpMutationCase mutation_cases[] = {
    {'V', XTP_MUTATION_VALID, XTP_EXPECT_VALID, "valid"},
    {'S', XTP_MUTATION_SCHEMA_VERSION, XTP_EXPECT_DECODE_REJECTION, "schema-version"},
    {'D', XTP_MUTATION_FULL_DIGEST, XTP_EXPECT_DECODE_REJECTION, "full-digest"},
    {'I', XTP_MUTATION_SEMANTIC_IDENTITY, XTP_EXPECT_MATERIALIZE_REJECTION,
     "semantic-identity"},
    {'P', XTP_MUTATION_PLAN_FINGERPRINT, XTP_EXPECT_MATERIALIZE_REJECTION,
     "plan-fingerprint"},
    {'B', XTP_MUTATION_SECTION_BOUNDS, XTP_EXPECT_DECODE_REJECTION, "section-bounds"},
    {'L', XTP_MUTATION_SECTION_LENGTH, XTP_EXPECT_DECODE_REJECTION, "section-length"},
    {'C', XTP_MUTATION_SECTION_COUNT, XTP_EXPECT_DECODE_REJECTION, "section-count"},
    {'Q', XTP_MUTATION_SECTION_ORDER, XTP_EXPECT_DECODE_REJECTION, "section-order"},
    {'G', XTP_MUTATION_SECTION_DIGEST, XTP_EXPECT_DECODE_REJECTION, "section-digest"},
    {'O', XTP_MUTATION_UNKNOWN_OPCODE, XTP_EXPECT_MATERIALIZE_REJECTION,
     "unknown-opcode"},
    {'T', XTP_MUTATION_UNKNOWN_RUNTIME_TAG, XTP_EXPECT_MATERIALIZE_REJECTION,
     "unknown-runtime-tag"},
    {'K', XTP_MUTATION_UNKNOWN_CONSTANT_FORM, XTP_EXPECT_MATERIALIZE_REJECTION,
     "unknown-constant-form"},
    {'R', XTP_MUTATION_TOTAL_ROWS_BUDGET, XTP_EXPECT_DECODE_REJECTION,
     "total-rows-budget"},
    {'W', XTP_MUTATION_VERIFY_WORK_BUDGET, XTP_EXPECT_DECODE_REJECTION,
     "verify-work-budget"},
};

static XtpFuzzFixture fixture;
static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};

static void require_fuzz(bool condition) {
    if (!condition)
        abort();
}

static void dispose_fixture(void) {
    xr_runtime_artifact_authority_free(fixture.authority);
    xr_xtp_encoded_free(fixture.bytes);
    xr_target_plan_free(fixture.plan);
    xr_target_profile_free(fixture.profile);
    xr_semantic_plan_free(fixture.semantic);
    memset(&fixture, 0, sizeof(fixture));
}

static bool initialize_fixture(void) {
    if (fixture.ready)
        return true;
    XiFunc *function = xi_func_new("xtp_fuzz", &stub_int);
    if (!function)
        return false;
    XiBlock *entry = xi_block_new(function);
    XiValue *result = entry ? xi_const_int(function, entry, 7, &stub_int) : NULL;
    if (!entry || !result) {
        xi_func_free(function);
        return false;
    }
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    char error[512] = {0};
    if (!xr_semantic_plan_build(function, &fixture.semantic, error, sizeof(error))) {
        xi_func_free(function);
        return false;
    }
    xi_func_free(function);
    fixture.profile = xr_test_target_profile_build(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    if (!fixture.profile ||
        !xr_target_plan_build(fixture.semantic, fixture.profile, &fixture.plan, error,
                              sizeof(error)) ||
        !xr_xtp_encode_plan(fixture.plan, &fixture.bytes, &fixture.size, error,
                            sizeof(error)) ||
        !xr_runtime_artifact_authority_create_internal(
            fixture.semantic, &fixture.authority, error, sizeof(error))) {
        dispose_fixture();
        return false;
    }
    fixture.ready = true;
    return true;
}

static uint8_t *directory_entry(uint8_t *bytes, XrXtpSectionKind kind) {
    return bytes + XR_XTP_HEADER_SIZE +
           ((size_t) kind - 1u) * XR_XTP_DIRECTORY_ENTRY_SIZE;
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

static void resign_section(uint8_t *bytes, XrXtpSectionKind kind) {
    uint8_t *entry = directory_entry(bytes, kind);
    size_t offset = (size_t) xr_xtp_take_u64(entry + 8);
    size_t length = (size_t) xr_xtp_take_u64(entry + 16);
    xr_sha256(bytes + offset, length, entry + 40);
}

static uint8_t *section_bytes(uint8_t *bytes, XrXtpSectionKind kind) {
    return bytes + (size_t) xr_xtp_take_u64(directory_entry(bytes, kind) + 8);
}

static void resign_section_and_artifact(uint8_t *bytes, XrXtpSectionKind kind) {
    resign_section(bytes, kind);
    resign_artifact(bytes, fixture.size);
}

static void exercise_runtime_boundary(const uint8_t *bytes, size_t size,
                                      XtpMutationExpectation expectation) {
    XrTargetPlan *loaded = (XrTargetPlan *) (uintptr_t) 1;
    bool accepted = xr_runtime_target_plan_load(
        bytes, size, fixture.authority, &loaded, NULL, 0);
    if (expectation == XTP_EXPECT_VALID) {
        require_fuzz(accepted && loaded && xr_target_plan_is_verified(loaded));
        xr_target_plan_free(loaded);
        return;
    }
    /* The runtime loader exposes no provider, finalizer, or entry callback
     * before verification. Its fail-closed observable is therefore exact: an
     * invalid artifact cannot produce the verified plan required by any of
     * those later registration paths. */
    require_fuzz(!accepted && loaded == NULL);
}

static void exercise_artifact(const uint8_t *bytes, size_t size,
                              XtpMutationExpectation expectation) {
    XrXtpCandidate *candidate = NULL;
    if (!xr_xtp_decode_candidate(bytes, size, &candidate, NULL, 0)) {
        require_fuzz(candidate == NULL);
        require_fuzz(expectation == XTP_EXPECT_ANY ||
                     expectation == XTP_EXPECT_DECODE_REJECTION);
        if (expectation != XTP_EXPECT_ANY)
            exercise_runtime_boundary(bytes, size, expectation);
        return;
    }
    require_fuzz(expectation != XTP_EXPECT_DECODE_REJECTION);
    XrTargetPlan *plan = NULL;
    if (xr_xtp_materialize_target_plan(candidate, fixture.semantic, fixture.profile, &plan,
                                        NULL, 0)) {
        uint8_t *roundtrip = NULL;
        size_t roundtrip_size = 0;
        require_fuzz(xr_target_plan_is_verified(plan));
        require_fuzz(xr_xtp_encode_plan(plan, &roundtrip, &roundtrip_size, NULL, 0));
        require_fuzz(roundtrip_size == size && memcmp(roundtrip, bytes, size) == 0);
        xr_xtp_encoded_free(roundtrip);
        xr_target_plan_free(plan);
        require_fuzz(expectation == XTP_EXPECT_ANY ||
                     expectation == XTP_EXPECT_VALID);
    } else {
        require_fuzz(plan == NULL);
        require_fuzz(expectation == XTP_EXPECT_ANY ||
                     expectation == XTP_EXPECT_MATERIALIZE_REJECTION);
    }
    xr_xtp_candidate_release(candidate);
    if (expectation != XTP_EXPECT_ANY && expectation != XTP_EXPECT_VALID)
        exercise_runtime_boundary(bytes, size, expectation);
}

static const XtpMutationCase *select_mutation(const uint8_t *data, size_t size) {
    require_fuzz(sizeof(mutation_cases) / sizeof(mutation_cases[0]) ==
                 XTP_MUTATION_COUNT);
    if (!size)
        return &mutation_cases[XTP_MUTATION_VALID];
    for (size_t i = 0; i < (size_t) XTP_MUTATION_COUNT; i++) {
        if (data[0] == mutation_cases[i].seed_code)
            return &mutation_cases[i];
    }
    return &mutation_cases[data[0] % XTP_MUTATION_COUNT];
}

static void apply_mutation(uint8_t *bytes, XtpMutation mutation) {
    uint8_t *entry = NULL;
    uint8_t *rows = NULL;
    switch (mutation) {
        case XTP_MUTATION_VALID: return;
        case XTP_MUTATION_SCHEMA_VERSION:
            xr_xtp_put_u32(bytes + 4, 0);
            resign_artifact(bytes, fixture.size);
            return;
        case XTP_MUTATION_FULL_DIGEST:
            bytes[XR_XTP_FULL_DIGEST_OFFSET] ^= 1u;
            return;
        case XTP_MUTATION_SEMANTIC_IDENTITY:
            bytes[72] ^= 1u;
            resign_artifact(bytes, fixture.size);
            return;
        case XTP_MUTATION_PLAN_FINGERPRINT:
            bytes[168] ^= 1u;
            resign_artifact(bytes, fixture.size);
            return;
        case XTP_MUTATION_SECTION_BOUNDS:
            entry = directory_entry(bytes, XR_XTP_SECTION_FUNCTIONS);
            xr_xtp_put_u64(entry + 8, UINT64_MAX);
            resign_artifact(bytes, fixture.size);
            return;
        case XTP_MUTATION_SECTION_LENGTH:
            entry = directory_entry(bytes, XR_XTP_SECTION_FUNCTIONS);
            xr_xtp_put_u64(entry + 16, xr_xtp_take_u64(entry + 16) + 1u);
            resign_artifact(bytes, fixture.size);
            return;
        case XTP_MUTATION_SECTION_COUNT:
            entry = directory_entry(bytes, XR_XTP_SECTION_FUNCTIONS);
            xr_xtp_put_u64(entry + 24,
                           xr_xtp_table_count_limit(XR_XTP_SECTION_FUNCTIONS) + 1u);
            resign_artifact(bytes, fixture.size);
            return;
        case XTP_MUTATION_SECTION_ORDER:
            entry = directory_entry(bytes, XR_XTP_SECTION_TARGET_PROFILE);
            xr_xtp_put_u32(entry, XR_XTP_SECTION_MACHINE_REPS);
            resign_artifact(bytes, fixture.size);
            return;
        case XTP_MUTATION_SECTION_DIGEST:
            entry = directory_entry(bytes, XR_XTP_SECTION_SLOTS);
            entry[40] ^= 1u;
            resign_artifact(bytes, fixture.size);
            return;
        case XTP_MUTATION_UNKNOWN_OPCODE:
            rows = section_bytes(bytes, XR_XTP_SECTION_INSTRUCTIONS);
            require_fuzz(rows[28] == XR_TARGET_INSTRUCTION_CONST_I64);
            rows[28] = UINT8_MAX;
            resign_section_and_artifact(bytes, XR_XTP_SECTION_INSTRUCTIONS);
            return;
        case XTP_MUTATION_UNKNOWN_RUNTIME_TAG:
            rows = section_bytes(bytes, XR_XTP_SECTION_TARGET_PROFILE);
            rows[296] = UINT8_MAX;
            resign_section_and_artifact(bytes, XR_XTP_SECTION_TARGET_PROFILE);
            return;
        case XTP_MUTATION_UNKNOWN_CONSTANT_FORM:
            rows = section_bytes(bytes, XR_XTP_SECTION_INSTRUCTIONS);
            require_fuzz(rows[28] == XR_TARGET_INSTRUCTION_CONST_I64);
            /* Constants have no kind selector in XTP: the opcode and canonical
             * zero-operand row are their complete form. A foreign constant
             * form is therefore represented by a noncanonical operand shape. */
            rows[29] = 1u;
            resign_section_and_artifact(bytes, XR_XTP_SECTION_INSTRUCTIONS);
            return;
        case XTP_MUTATION_TOTAL_ROWS_BUDGET:
            xr_xtp_put_u64(bytes + 296, XR_XTP_MAX_TOTAL_ROWS + 1u);
            resign_artifact(bytes, fixture.size);
            return;
        case XTP_MUTATION_VERIFY_WORK_BUDGET:
            xr_xtp_put_u64(bytes + 320, XR_XTP_MAX_VERIFY_WORK_UNITS + 1u);
            resign_artifact(bytes, fixture.size);
            return;
        case XTP_MUTATION_COUNT: break;
    }
    require_fuzz(false);
}

static void structured_mutation(const uint8_t *data, size_t size) {
    uint8_t *bytes = (uint8_t *) xr_malloc(fixture.size);
    if (!bytes)
        return;
    memcpy(bytes, fixture.bytes, fixture.size);
    const XtpMutationCase *mutation = select_mutation(data, size);
    apply_mutation(bytes, mutation->mutation);
    exercise_artifact(bytes, fixture.size, mutation->expectation);
    xr_free(bytes);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!initialize_fixture())
        return 0;
    exercise_artifact(data, size, XTP_EXPECT_ANY);
    structured_mutation(data, size);
    return 0;
}

#ifdef FUZZ_STANDALONE
int main(void) {
    require_fuzz(initialize_fixture());
    for (size_t i = 0; i < (size_t) XTP_MUTATION_COUNT; i++) {
        uint8_t selector = mutation_cases[i].seed_code;
        fprintf(stderr, "checking deterministic XTP mutation: %s\n",
                mutation_cases[i].name);
        structured_mutation(&selector, 1);
    }
    dispose_fixture();
    puts("typed XTP deterministic mutation matrix passed");
    return 0;
}
#endif
