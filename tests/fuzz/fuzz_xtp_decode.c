/*
 * fuzz_xtp_decode.c - Typed artifact decoder and verifier fuzz entry
 */

#include "../../src/ir/xi.h"
#include "../../src/plan/format/xr_xtp_internal.h"
#include "../../src/plan/semantic/xr_semantic_builder.h"
#include "../../src/plan/target/xr_target_builder.h"
#include "../../src/base/xmalloc.h"
#include "../../src/base/xsha256.h"
#include "../../src/runtime/value/xtype.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct XtpFuzzFixture {
    XrSemanticPlan *semantic;
    XrTargetProfile *profile;
    XrTargetPlan *plan;
    uint8_t *bytes;
    size_t size;
    bool ready;
} XtpFuzzFixture;

static XtpFuzzFixture fixture;
static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};

static void require_fuzz(bool condition) {
    if (!condition)
        abort();
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
    XrTargetProfileDraft profile = {0};
    profile.schema_version = XR_TARGET_PROFILE_SCHEMA_VERSION;
    profile.architecture = XR_TARGET_ARCH_X86_64;
    profile.operating_system = XR_TARGET_OS_WINDOWS;
    profile.environment = XR_TARGET_ENV_MSVC;
    profile.native_abi = XR_TARGET_ABI_WIN64_X86_64;
    profile.runtime_profile = XR_TARGET_RUNTIME_HOSTED;
    if (!xr_target_data_layout_init_lp64(&profile.data_layout))
        return false;
    profile.atomic_width_mask = XR_TARGET_ATOMIC_WIDTH_8 | XR_TARGET_ATOMIC_WIDTH_16 |
                                XR_TARGET_ATOMIC_WIDTH_32 | XR_TARGET_ATOMIC_WIDTH_64;
    profile.atomic_order_mask = XR_TARGET_ATOMIC_RELAXED | XR_TARGET_ATOMIC_ACQUIRE |
                                XR_TARGET_ATOMIC_RELEASE | XR_TARGET_ATOMIC_ACQ_REL |
                                XR_TARGET_ATOMIC_SEQ_CST;
    profile.float_feature_mask = XR_TARGET_FLOAT_IEEE754 | XR_TARGET_FLOAT_STRICT;
    profile.vector_feature_mask = XR_TARGET_VECTOR_SSE2;
    profile.maximum_vector_bits = 128;
    profile.provider_mask = UINT64_C(1) << XR_TARGET_PROVIDER_ALLOCATOR;
    profile.provider_set_fingerprint.bytes[0] = 0x31;
    profile.object_header_fingerprint.bytes[0] = 0x32;
    profile.runtime_abi_fingerprint.bytes[0] = 0x33;
    if (!xr_target_profile_freeze(&profile, &fixture.profile, error, sizeof(error)) ||
        !xr_target_plan_build(fixture.semantic, fixture.profile, &fixture.plan, error,
                              sizeof(error)) ||
        !xr_xtp_encode_plan(fixture.plan, &fixture.bytes, &fixture.size, error, sizeof(error)))
        return false;
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

static void exercise_artifact(const uint8_t *bytes, size_t size) {
    XrXtpCandidate *candidate = NULL;
    if (!xr_xtp_decode_candidate(bytes, size, &candidate, NULL, 0)) {
        require_fuzz(candidate == NULL);
        return;
    }
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
    } else {
        require_fuzz(plan == NULL);
    }
    xr_xtp_candidate_release(candidate);
}

static void structured_mutation(const uint8_t *data, size_t size) {
    uint8_t *bytes = (uint8_t *) xr_malloc(fixture.size);
    if (!bytes)
        return;
    memcpy(bytes, fixture.bytes, fixture.size);
    uint8_t selector = size ? data[0] : 0;
    switch (selector % 10u) {
        case 0: break;
        case 1: xr_xtp_put_u32(bytes + 4, 1); resign_artifact(bytes, fixture.size); break;
        case 2: bytes[XR_XTP_FULL_DIGEST_OFFSET] ^= 1; break;
        case 3: bytes[72] ^= 1; resign_artifact(bytes, fixture.size); break;
        case 4: {
            uint8_t *entry = directory_entry(bytes, XR_XTP_SECTION_FUNCTIONS);
            size_t offset = (size_t) xr_xtp_take_u64(entry + 8);
            bytes[offset + 16] ^= size > 1 ? data[1] | 1u : 1u;
            resign_section(bytes, XR_XTP_SECTION_FUNCTIONS);
            resign_artifact(bytes, fixture.size);
            break;
        }
        case 5: {
            uint8_t *entry = directory_entry(bytes, XR_XTP_SECTION_SLOTS);
            size_t offset = (size_t) xr_xtp_take_u64(entry + 8);
            bytes[offset] ^= size > 1 ? data[1] | 1u : 1u;
            resign_section(bytes, XR_XTP_SECTION_SLOTS);
            resign_artifact(bytes, fixture.size);
            break;
        }
        case 6: {
            uint8_t *entry = directory_entry(bytes, XR_XTP_SECTION_FUNCTIONS);
            xr_xtp_put_u64(entry + 8, UINT64_MAX);
            resign_artifact(bytes, fixture.size);
            break;
        }
        case 7: xr_xtp_put_u64(bytes + 48, 0); resign_artifact(bytes, fixture.size); break;
        case 8: xr_xtp_put_u64(bytes + 312, xr_xtp_take_u64(bytes + 312) + 1u);
                resign_artifact(bytes, fixture.size); break;
        case 9: {
            uint8_t *entry = directory_entry(bytes, XR_XTP_SECTION_SLOTS);
            entry[40] ^= 1;
            resign_artifact(bytes, fixture.size);
            break;
        }
    }
    exercise_artifact(bytes, fixture.size);
    xr_free(bytes);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!initialize_fixture())
        return 0;
    exercise_artifact(data, size);
    structured_mutation(data, size);
    return 0;
}

#ifdef FUZZ_STANDALONE
int main(void) {
    for (uint8_t selector = 0; selector < 32; selector++)
        LLVMFuzzerTestOneInput(&selector, 1);
    puts("typed XTP fuzz smoke passed");
    return 0;
}
#endif
