#include "runtime/abi/xr_runtime_descriptor.h"

#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition, message)                                                             \
    do {                                                                                      \
        if (!(condition)) {                                                                   \
            fprintf(stderr, "FAIL:%d: %s\n", __LINE__, (message));                          \
            failures++;                                                                       \
        }                                                                                     \
    } while (0)

_Static_assert(sizeof(XrStableId) == XR_STABLE_ID_BYTES, "stable IDs are byte exact");
_Static_assert(sizeof(XrFingerprint) == XR_FINGERPRINT_BYTES,
               "fingerprints are byte exact");
_Static_assert(offsetof(XrRuntimeExtentDescriptor, fingerprint) >
                   offsetof(XrRuntimeExtentDescriptor, kind),
               "extent fingerprint follows every hashed field");
_Static_assert(offsetof(XrRuntimeLayoutDescriptor, fingerprint) >
                   offsetof(XrRuntimeLayoutDescriptor, flags),
               "layout fingerprint follows every hashed field");

static XrStableId make_id(uint8_t seed) {
    XrStableId id = {{0}};
    for (size_t i = 0; i < sizeof(id.bytes); i++)
        id.bytes[i] = (uint8_t) (seed + i);
    return id;
}

static bool fingerprint_equal(XrFingerprint left, XrFingerprint right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

static void seal_extent(XrRuntimeExtentDescriptor *extent) {
    CHECK(xr_runtime_extent_descriptor_fingerprint(extent, &extent->fingerprint) ==
              XR_RUNTIME_ABI_OK,
          "extent sealing succeeds");
}

static void seal_layout(XrRuntimeLayoutDescriptor *layout) {
    CHECK(xr_runtime_layout_descriptor_fingerprint(layout, &layout->fingerprint) ==
              XR_RUNTIME_ABI_OK,
          "layout sealing succeeds");
}

static XrRuntimeExtentDescriptor make_extent(XrRuntimeExtentKind kind, uint8_t seed,
                                             XrStableId layout_id) {
    XrRuntimeExtentDescriptor extent = {
        .schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .id = make_id(seed),
        .layout_id = layout_id,
        .operand_index = XR_RUNTIME_EXTENT_OPERAND_NONE,
        .part_count = 1,
        .kind = (uint8_t) kind,
    };
    return extent;
}

static XrRuntimeLayoutDescriptor make_layout(uint8_t seed,
                                             const XrRuntimeExtentDescriptor *extent,
                                             uint64_t prefix, uint32_t alignment) {
    XrRuntimeLayoutDescriptor layout = {
        .schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .descriptor_id = make_id(seed),
        .layout_id = extent->layout_id,
        .object_kind_id = make_id((uint8_t) (seed + 1)),
        .extent_id = extent->id,
        .extent_fingerprint = extent->fingerprint,
        .fixed_prefix_size = prefix,
        .alignment = alignment,
        .allowed_semantic_domains =
            XR_SEMANTIC_DOMAIN_MASK(XR_STORAGE_EXEC_LOCAL) |
            XR_SEMANTIC_DOMAIN_MASK(XR_STORAGE_SYNC_SHARED),
        .allowed_materializations =
            XR_MATERIALIZATION_MASK(XR_MATERIALIZE_EXEC_HEAP) |
            XR_MATERIALIZATION_MASK(XR_MATERIALIZE_SYSTEM_HEAP),
    };
    seal_layout(&layout);
    return layout;
}

static void bind_extent(XrRuntimeLayoutDescriptor *layout,
                        XrRuntimeExtentDescriptor *extent) {
    seal_extent(extent);
    layout->extent_fingerprint = extent->fingerprint;
    seal_layout(layout);
}

static XrRuntimeAbiStatus provider_ok(XrStableId provider_id, const uint64_t *operands,
                                      size_t operand_count, uint64_t *out_bytes,
                                      void *context) {
    (void) operands;
    (void) operand_count;
    if (provider_id.bytes[0] != 90 || !context)
        return XR_RUNTIME_ABI_PROVIDER_REJECTED;
    *out_bytes = *(const uint64_t *) context;
    return XR_RUNTIME_ABI_OK;
}

static XrRuntimeAbiStatus provider_reject(XrStableId provider_id,
                                          const uint64_t *operands,
                                          size_t operand_count, uint64_t *out_bytes,
                                          void *context) {
    (void) provider_id;
    (void) operands;
    (void) operand_count;
    (void) out_bytes;
    (void) context;
    return XR_RUNTIME_ABI_INVALID_EXTENT;
}

static int provider_call_count;

static XrRuntimeAbiStatus provider_counting(XrStableId provider_id,
                                            const uint64_t *operands,
                                            size_t operand_count, uint64_t *out_bytes,
                                            void *context) {
    provider_call_count++;
    return provider_ok(provider_id, operands, operand_count, out_bytes, context);
}

static void expect_bytes(const XrRuntimeExtentDescriptor *extent,
                         const XrRuntimeLayoutDescriptor *layout, const uint64_t *operands,
                         size_t operand_count, XrRuntimeExtentProviderEvaluateFn provider,
                         void *context, uint64_t expected) {
    XrRuntimeEvaluatedExtent evaluated;
    XrRuntimeExtentLimits limits = {.max_allocation_bytes = UINT64_C(1) << 40,
                                    .max_alignment = 4096};
    CHECK(xr_runtime_extent_evaluate(extent, layout, operands, operand_count, limits,
                                     provider, context, &evaluated) == XR_RUNTIME_ABI_OK,
          "extent evaluation succeeds");
    CHECK(evaluated.bytes == expected, "evaluated bytes match canonical formula");
    CHECK(fingerprint_equal(evaluated.extent_fingerprint, extent->fingerprint),
          "evaluated extent preserves source fingerprint");
}

static void test_all_extent_kinds(void) {
    XrStableId fixed_layout_id = make_id(20);
    XrRuntimeExtentDescriptor fixed =
        make_extent(XR_RUNTIME_EXTENT_FIXED, 10, fixed_layout_id);
    seal_extent(&fixed);
    XrRuntimeLayoutDescriptor fixed_layout = make_layout(30, &fixed, 24, 8);
    CHECK(xr_runtime_layout_descriptor_verify(&fixed_layout, &fixed) == XR_RUNTIME_ABI_OK,
          "fixed descriptor pair verifies");
    expect_bytes(&fixed, &fixed_layout, NULL, 0, NULL, NULL, 24);

    XrStableId inline_layout_id = make_id(40);
    XrRuntimeExtentDescriptor inline_tail =
        make_extent(XR_RUNTIME_EXTENT_INLINE_TAIL, 41, inline_layout_id);
    inline_tail.tail_offset = 32;
    inline_tail.stride = 5;
    inline_tail.operand_index = 0;
    seal_extent(&inline_tail);
    XrRuntimeLayoutDescriptor inline_layout = make_layout(42, &inline_tail, 24, 8);
    uint64_t three[] = {3};
    expect_bytes(&inline_tail, &inline_layout, three, 1, NULL, NULL, 48);

    XrStableId buffer_layout_id = make_id(50);
    XrRuntimeExtentDescriptor external =
        make_extent(XR_RUNTIME_EXTENT_EXTERNAL_BUFFER, 51, buffer_layout_id);
    external.stride = 4;
    external.operand_index = 0;
    seal_extent(&external);
    XrRuntimeLayoutDescriptor buffer_layout = make_layout(52, &external, 0, 16);
    expect_bytes(&external, &buffer_layout, three, 1, NULL, NULL, 16);

    XrStableId multi_layout_id = make_id(60);
    XrRuntimeExtentDescriptor multi =
        make_extent(XR_RUNTIME_EXTENT_MULTI_BUFFER, 61, multi_layout_id);
    multi.group_id = make_id(62);
    multi.tail_offset = 16;
    multi.stride = 8;
    multi.operand_index = 0;
    multi.part_index = 1;
    multi.part_count = 2;
    seal_extent(&multi);
    XrRuntimeLayoutDescriptor multi_layout = make_layout(63, &multi, 8, 16);
    uint64_t two[] = {2};
    expect_bytes(&multi, &multi_layout, two, 1, NULL, NULL, 32);

    XrStableId provider_layout_id = make_id(70);
    XrRuntimeExtentDescriptor provider =
        make_extent(XR_RUNTIME_EXTENT_PROVIDER_DEFINED, 71, provider_layout_id);
    provider.provider_id = make_id(90);
    seal_extent(&provider);
    XrRuntimeLayoutDescriptor provider_layout = make_layout(72, &provider, 12, 4);
    uint64_t provided = 37;
    expect_bytes(&provider, &provider_layout, NULL, 0, provider_ok, &provided, 40);
}

static void test_descriptor_mutations_fail_closed(void) {
    XrStableId layout_id = make_id(100);
    XrRuntimeExtentDescriptor extent =
        make_extent(XR_RUNTIME_EXTENT_INLINE_TAIL, 101, layout_id);
    extent.tail_offset = 16;
    extent.stride = 8;
    extent.operand_index = 0;
    seal_extent(&extent);
    XrRuntimeLayoutDescriptor layout = make_layout(102, &extent, 16, 8);

    XrRuntimeExtentDescriptor mutated_extent = extent;
    mutated_extent.stride++;
    CHECK(xr_runtime_extent_descriptor_verify(&mutated_extent, &layout) ==
              XR_RUNTIME_ABI_FINGERPRINT_MISMATCH,
          "extent mutation invalidates fingerprint");

    XrRuntimeLayoutDescriptor mutated_layout = layout;
    mutated_layout.alignment = 3;
    CHECK(xr_runtime_layout_descriptor_verify(&mutated_layout, &extent) ==
              XR_RUNTIME_ABI_INVALID_ALIGNMENT,
          "non-power-of-two alignment is rejected before fingerprint trust");

    mutated_layout = layout;
    mutated_layout.extent_fingerprint.bytes[0] ^= 1;
    seal_layout(&mutated_layout);
    CHECK(xr_runtime_layout_descriptor_verify(&mutated_layout, &extent) ==
              XR_RUNTIME_ABI_INVALID_EXTENT,
          "layout cannot bind a stale extent fingerprint");

    mutated_layout = layout;
    mutated_layout.allowed_semantic_domains |=
        XR_SEMANTIC_DOMAIN_MASK(XR_STORAGE_DOMAIN_UNKNOWN);
    seal_layout(&mutated_layout);
    CHECK(xr_runtime_layout_descriptor_verify(&mutated_layout, &extent) ==
              XR_RUNTIME_ABI_INVALID_DOMAIN,
          "unknown semantic domain is not an allowlist category");

    mutated_layout = layout;
    memset(&mutated_layout.object_kind_id, 0, sizeof(mutated_layout.object_kind_id));
    seal_layout(&mutated_layout);
    CHECK(xr_runtime_layout_descriptor_verify(&mutated_layout, &extent) ==
              XR_RUNTIME_ABI_INVALID_IDENTITY,
          "zero stable object-kind identity is rejected");
}

static void test_extent_arithmetic_fail_closed(void) {
    XrStableId layout_id = make_id(120);
    XrRuntimeExtentDescriptor extent =
        make_extent(XR_RUNTIME_EXTENT_EXTERNAL_BUFFER, 121, layout_id);
    extent.tail_offset = 8;
    extent.stride = UINT64_MAX;
    extent.operand_index = 0;
    seal_extent(&extent);
    XrRuntimeLayoutDescriptor layout = make_layout(122, &extent, 8, 8);
    XrRuntimeExtentLimits limits = {.max_allocation_bytes = UINT64_MAX,
                                    .max_alignment = 4096};
    XrRuntimeEvaluatedExtent evaluated;
    uint64_t two[] = {2};
    CHECK(xr_runtime_extent_evaluate(&extent, &layout, two, 1, limits, NULL, NULL,
                                     &evaluated) == XR_RUNTIME_ABI_OVERFLOW,
          "count times stride overflow is rejected");

    extent.stride = 1;
    extent.tail_offset = UINT64_MAX;
    bind_extent(&layout, &extent);
    uint64_t one[] = {1};
    CHECK(xr_runtime_extent_evaluate(&extent, &layout, one, 1, limits, NULL, NULL,
                                     &evaluated) == XR_RUNTIME_ABI_OVERFLOW,
          "tail offset addition overflow is rejected");

    extent.tail_offset = 0;
    extent.stride = 0;
    extent.kind = XR_RUNTIME_EXTENT_FIXED;
    extent.operand_index = XR_RUNTIME_EXTENT_OPERAND_NONE;
    layout.fixed_prefix_size = UINT64_MAX - 6;
    bind_extent(&layout, &extent);
    CHECK(xr_runtime_extent_evaluate(&extent, &layout, NULL, 0, limits, NULL, NULL,
                                     &evaluated) == XR_RUNTIME_ABI_OVERFLOW,
          "alignment rounding overflow is rejected");

    extent = make_extent(XR_RUNTIME_EXTENT_INLINE_TAIL, 123, layout_id);
    extent.tail_offset = 8;
    extent.stride = 4;
    extent.operand_index = 2;
    seal_extent(&extent);
    layout = make_layout(124, &extent, 8, 8);
    CHECK(xr_runtime_extent_evaluate(&extent, &layout, one, 1, limits, NULL, NULL,
                                     &evaluated) == XR_RUNTIME_ABI_INVALID_ARGUMENT,
          "missing extent operand is rejected");

    extent.operand_index = 0;
    bind_extent(&layout, &extent);
    limits.max_allocation_bytes = 8;
    CHECK(xr_runtime_extent_evaluate(&extent, &layout, one, 1, limits, NULL, NULL,
                                     &evaluated) == XR_RUNTIME_ABI_LIMIT_EXCEEDED,
          "allocation byte limit is enforced");
    limits.max_allocation_bytes = 1024;
    limits.max_alignment = 4;
    CHECK(xr_runtime_extent_evaluate(&extent, &layout, one, 1, limits, NULL, NULL,
                                     &evaluated) == XR_RUNTIME_ABI_LIMIT_EXCEEDED,
          "alignment limit is enforced");
}

static void test_provider_and_domain_checks(void) {
    XrStableId layout_id = make_id(140);
    XrRuntimeExtentDescriptor extent =
        make_extent(XR_RUNTIME_EXTENT_PROVIDER_DEFINED, 141, layout_id);
    extent.provider_id = make_id(90);
    seal_extent(&extent);
    XrRuntimeLayoutDescriptor layout = make_layout(142, &extent, 16, 8);
    XrRuntimeExtentLimits limits = {.max_allocation_bytes = 1024, .max_alignment = 64};
    XrRuntimeEvaluatedExtent evaluated;
    CHECK(xr_runtime_extent_evaluate(&extent, &layout, NULL, 0, limits, NULL, NULL,
                                     &evaluated) == XR_RUNTIME_ABI_PROVIDER_REQUIRED,
          "provider-defined extent requires a provider");
    CHECK(xr_runtime_extent_evaluate(&extent, &layout, NULL, 0, limits, provider_reject,
                                     NULL, &evaluated) == XR_RUNTIME_ABI_PROVIDER_REJECTED,
          "provider rejection is normalized");
    uint64_t provided = 32;
    provider_call_count = 0;
    CHECK(xr_runtime_extent_evaluate(&extent, &layout, NULL, 1, limits,
                                     provider_counting, &provided, &evaluated) ==
              XR_RUNTIME_ABI_INVALID_ARGUMENT && provider_call_count == 0,
          "null nonempty operand vector is rejected before provider dispatch");
    uint64_t too_small = 8;
    CHECK(xr_runtime_extent_evaluate(&extent, &layout, NULL, 0, limits, provider_ok,
                                     &too_small, &evaluated) == XR_RUNTIME_ABI_INVALID_EXTENT,
          "provider cannot undercut its allocation layout prefix");

    XrRuntimeDomainIdentity local = {.contract_id = make_id(150),
                                     .instance_id = 1,
                                     .semantic_domain = XR_STORAGE_EXEC_LOCAL,
                                     .materialization = XR_MATERIALIZE_EXEC_HEAP};
    XrRuntimeDomainIdentity other_instance = local;
    other_instance.instance_id = 2;
    CHECK(xr_runtime_layout_allows_domain(&layout, local),
          "layout accepts independently modeled domain axes");
    CHECK(!xr_runtime_domain_identity_equal(local, other_instance),
          "same category with another runtime instance is not the same domain");
    other_instance = local;
    other_instance.contract_id = make_id(151);
    CHECK(!xr_runtime_domain_identity_equal(local, other_instance),
          "same category with another contract identity is not the same domain");
}

static void test_fingerprint_known_answer(void) {
    XrStableId layout_id = make_id(160);
    XrRuntimeExtentDescriptor extent =
        make_extent(XR_RUNTIME_EXTENT_INLINE_TAIL, 161, layout_id);
    extent.tail_offset = 48;
    extent.stride = 7;
    extent.operand_index = 3;
    seal_extent(&extent);
    static const uint8_t expected_extent[XR_FINGERPRINT_BYTES] = {
        0xf3, 0x2d, 0x2e, 0xec, 0x65, 0x53, 0x4a, 0xe4, 0x53, 0x76, 0xb4,
        0xed, 0x5a, 0x37, 0x61, 0x55, 0x29, 0xb0, 0xea, 0xbe, 0xd6, 0x24,
        0x94, 0x0a, 0xd7, 0x6f, 0xb6, 0x51, 0x6b, 0xcc, 0x12, 0xce,
    };
    CHECK(memcmp(extent.fingerprint.bytes, expected_extent, sizeof(expected_extent)) == 0,
          "extent fingerprint matches the frozen known answer");

    XrRuntimeLayoutDescriptor layout = make_layout(162, &extent, 40, 16);
    layout.root_plan_id = make_id(163);
    layout.destructor_id = make_id(164);
    layout.clone_id = make_id(165);
    layout.eq_hash_id = make_id(166);
    layout.flags = XR_LAYOUT_HAS_ROOTS | XR_LAYOUT_HAS_DESTRUCTOR | XR_LAYOUT_HAS_CLONE |
                   XR_LAYOUT_HAS_EQ_HASH;
    seal_layout(&layout);
    static const uint8_t expected_layout[XR_FINGERPRINT_BYTES] = {
        0x80, 0xcb, 0xe0, 0x27, 0x61, 0xf1, 0x4d, 0x64, 0xf3, 0xd4, 0x9b,
        0x66, 0xc3, 0x5e, 0xca, 0xaa, 0xa6, 0x71, 0x67, 0xec, 0x86, 0x69,
        0x20, 0xf8, 0x81, 0x02, 0x52, 0x99, 0x95, 0x6b, 0x5f, 0x62,
    };
    CHECK(memcmp(layout.fingerprint.bytes, expected_layout, sizeof(expected_layout)) == 0,
          "layout fingerprint matches the frozen known answer");
}

static void test_multi_buffer_group_verifier(void) {
    XrStableId group_id = make_id(200);
    XrStableId layout_zero_id = make_id(201);
    XrRuntimeExtentDescriptor extent_zero =
        make_extent(XR_RUNTIME_EXTENT_MULTI_BUFFER, 202, layout_zero_id);
    extent_zero.group_id = group_id;
    extent_zero.tail_offset = 8;
    extent_zero.stride = 4;
    extent_zero.operand_index = 0;
    extent_zero.part_index = 0;
    extent_zero.part_count = 2;
    seal_extent(&extent_zero);
    XrRuntimeLayoutDescriptor layout_zero = make_layout(203, &extent_zero, 8, 8);

    XrStableId layout_one_id = make_id(211);
    XrRuntimeExtentDescriptor extent_one =
        make_extent(XR_RUNTIME_EXTENT_MULTI_BUFFER, 212, layout_one_id);
    extent_one.group_id = group_id;
    extent_one.tail_offset = 16;
    extent_one.stride = 8;
    extent_one.operand_index = 1;
    extent_one.part_index = 1;
    extent_one.part_count = 2;
    seal_extent(&extent_one);
    XrRuntimeLayoutDescriptor layout_one = make_layout(213, &extent_one, 16, 16);

    XrRuntimeExtentGroupEntry reversed[] = {
        {.extent = &extent_one, .layout = &layout_one},
        {.extent = &extent_zero, .layout = &layout_zero},
    };
    XrRuntimeExtentGroupSummary summary;
    CHECK(xr_runtime_extent_group_verify(reversed, 2, &summary) == XR_RUNTIME_ABI_OK,
          "complete multi-buffer group verifies independent of input order");
    CHECK(memcmp(summary.group_id.bytes, group_id.bytes, sizeof(group_id.bytes)) == 0 &&
              summary.part_count == 2,
          "group summary preserves canonical identity and part count");
    static const uint8_t expected_group[XR_FINGERPRINT_BYTES] = {
        0xbb, 0x8f, 0x42, 0xce, 0x23, 0x59, 0x7d, 0x44,
        0x3a, 0x7c, 0x22, 0x93, 0x08, 0xe1, 0xc6, 0xa8,
        0x73, 0x43, 0x4b, 0x15, 0xd5, 0x7b, 0xed, 0x10,
        0xd9, 0x68, 0xe9, 0xd8, 0xaa, 0xed, 0x8e, 0x6c,
    };
    CHECK(memcmp(summary.fingerprint.bytes, expected_group, sizeof(expected_group)) == 0,
          "group summary fingerprint matches the frozen known answer");

    XrRuntimeExtentGroupEntry ordered[] = {
        {.extent = &extent_zero, .layout = &layout_zero},
        {.extent = &extent_one, .layout = &layout_one},
    };
    XrRuntimeExtentGroupSummary ordered_summary;
    CHECK(xr_runtime_extent_group_verify(ordered, 2, &ordered_summary) == XR_RUNTIME_ABI_OK &&
              fingerprint_equal(summary.fingerprint, ordered_summary.fingerprint),
          "group fingerprint is canonical across entry permutations");

    XrRuntimeExtentDescriptor mutated_extent = extent_one;
    XrRuntimeLayoutDescriptor mutated_layout = layout_one;
    XrRuntimeExtentGroupEntry mutated[] = {
        {.extent = &extent_zero, .layout = &layout_zero},
        {.extent = &mutated_extent, .layout = &mutated_layout},
    };
    mutated_extent.part_index = 0;
    bind_extent(&mutated_layout, &mutated_extent);
    CHECK(xr_runtime_extent_group_verify(mutated, 2, &ordered_summary) ==
              XR_RUNTIME_ABI_INVALID_GROUP,
          "duplicate group part index is rejected");

    mutated_extent = extent_one;
    mutated_layout = layout_one;
    mutated_extent.part_count = 3;
    mutated_extent.part_index = 2;
    bind_extent(&mutated_layout, &mutated_extent);
    CHECK(xr_runtime_extent_group_verify(mutated, 2, &ordered_summary) ==
              XR_RUNTIME_ABI_INVALID_GROUP,
          "missing dense group part is rejected");

    mutated_extent = extent_one;
    mutated_layout = layout_one;
    mutated_extent.group_id = make_id(220);
    bind_extent(&mutated_layout, &mutated_extent);
    CHECK(xr_runtime_extent_group_verify(mutated, 2, &ordered_summary) ==
              XR_RUNTIME_ABI_INVALID_GROUP,
          "foreign group identity is rejected");

    mutated_extent = extent_one;
    mutated_layout = layout_one;
    mutated_extent.id = extent_zero.id;
    mutated_layout.extent_id = mutated_extent.id;
    bind_extent(&mutated_layout, &mutated_extent);
    CHECK(xr_runtime_extent_group_verify(mutated, 2, &ordered_summary) ==
              XR_RUNTIME_ABI_INVALID_GROUP,
          "duplicate physical extent identity is rejected");

    mutated_extent = extent_one;
    mutated_layout = layout_one;
    mutated_layout.descriptor_id = layout_zero.descriptor_id;
    seal_layout(&mutated_layout);
    CHECK(xr_runtime_extent_group_verify(mutated, 2, &ordered_summary) ==
              XR_RUNTIME_ABI_INVALID_GROUP,
          "duplicate runtime descriptor identity is rejected");

    mutated_extent = extent_one;
    mutated_layout = layout_one;
    mutated_extent.stride++;
    CHECK(xr_runtime_extent_group_verify(mutated, 2, &ordered_summary) ==
              XR_RUNTIME_ABI_FINGERPRINT_MISMATCH,
          "stale member fingerprint poisons group verification");

    memset(&ordered_summary, 0x5a, sizeof(ordered_summary));
    CHECK(xr_runtime_extent_group_verify(ordered, 1, &ordered_summary) ==
              XR_RUNTIME_ABI_INVALID_GROUP,
          "partial single-entry group is rejected");
    CHECK(ordered_summary.fingerprint.bytes[0] == 0x5a,
          "failed group verification does not publish a partial summary");
    mutated[1].extent = NULL;
    CHECK(xr_runtime_extent_group_verify(mutated, 2, &ordered_summary) ==
              XR_RUNTIME_ABI_INVALID_ARGUMENT,
          "null group member is rejected");
}

int main(void) {
    test_all_extent_kinds();
    test_descriptor_mutations_fail_closed();
    test_extent_arithmetic_fail_closed();
    test_provider_and_domain_checks();
    test_fingerprint_known_answer();
    test_multi_buffer_group_verifier();
    if (failures != 0) {
        fprintf(stderr, "%d runtime descriptor test(s) failed\n", failures);
        return 1;
    }
    puts("runtime descriptor tests passed");
    return 0;
}
