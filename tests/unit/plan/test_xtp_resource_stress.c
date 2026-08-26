/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xtp_resource_stress.c - Bounded standalone XTP stress coverage
 *
 * KEY CONCEPT:
 *   Valid artifacts grow through a deterministic CFG and table-size ladder.
 *   Oversized manifests and typed-row mutations fail before any runtime
 *   generation or entry-cell registration can become observable.
 */

#include "../../../include/xray_runtime_generation.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/base/xplatform.h"
#include "../../../src/base/xsha256.h"
#include "../../../src/ir/xi.h"
#include "../../../src/os/os_thread.h"
#include "../../../src/os/os_time.h"
#include "../../../src/plan/format/xr_xtp_internal.h"
#include "../../../src/plan/semantic/xr_semantic_builder.h"
#include "../../../src/plan/target/xr_target_builder.h"
#include "../../../src/plan/target/xr_target_profile_internal.h"
#include "../../../src/runtime/abi/xr_runtime_target_authority.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/runtime/xr_entry_cell.h"
#include "../../../src/runtime/xr_module_generation_internal.h"

#ifdef XR_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

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

#define XTP_LADDER_MAX_WALL_NS UINT64_C(30000000000)

typedef struct ArtifactFixture {
    XrSemanticPlan *semantic;
    XrTargetProfile *profile;
    XrTargetPlan *plan;
    uint8_t *bytes;
    size_t size;
} ArtifactFixture;

typedef struct ConcurrentLoadContext {
    const ArtifactFixture *fixture;
    XrXtpCandidate *shared_candidate;
    XrLoadedModuleGeneration *generation;
    XrFingerprint expected_fingerprint;
    bool completed;
} ConcurrentLoadContext;

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};

static uint64_t current_process_peak_bytes(void) {
#ifdef XR_OS_WINDOWS
    PROCESS_MEMORY_COUNTERS counters;
    memset(&counters, 0, sizeof(counters));
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters,
                              sizeof(counters)))
        return 0;
    return (uint64_t) counters.PeakWorkingSetSize;
#else
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss <= 0)
        return 0;
#ifdef XR_OS_MACOS
    return (uint64_t) usage.ru_maxrss;
#else
    return (uint64_t) usage.ru_maxrss * UINT64_C(1024);
#endif
#endif
}

static XrTargetProfile *build_native_profile(void) {
    XrRuntimeTargetAuthority authority;
    REQUIRE(xr_runtime_target_authority_native_hosted(&authority) ==
            XR_RUNTIME_ABI_OK);
    XrTargetProfileBuildInput input = {
        .machine = authority.machine,
        .runtime_abi = &authority.runtime_abi,
        .object_header_materialization =
            &authority.object_header_materialization,
        .string_contract = &authority.string_contract,
        .providers = authority.providers,
        .provider_count = authority.provider_count,
    };
    XrTargetProfile *profile = NULL;
    char diagnostic[512] = {0};
    REQUIRE(xr_target_profile_build(&input, &profile, diagnostic,
                                    sizeof(diagnostic)));
    return profile;
}

static XrSemanticPlan *build_ladder_semantic(uint32_t block_count,
                                             uint32_t operations_per_block) {
    REQUIRE(block_count != 0 && operations_per_block != 0);
    XiFunc *function = xi_func_new("xtp_resource_ladder", &stub_int);
    REQUIRE(function != NULL);
    XiBlock **blocks =
        (XiBlock **) xr_calloc(block_count, sizeof(*blocks));
    REQUIRE(blocks != NULL);
    for (uint32_t i = 0; i < block_count; i++) {
        blocks[i] = xi_block_new(function);
        REQUIRE(blocks[i] != NULL);
        blocks[i]->sealed = true;
    }

    XiValue *current = xi_const_int(function, blocks[0], 1, &stub_int);
    REQUIRE(current != NULL);
    for (uint32_t block = 0; block < block_count; block++) {
        for (uint32_t operation = 0; operation < operations_per_block;
             operation++) {
            XiValue *increment =
                xi_const_int(function, blocks[block], 1, &stub_int);
            XiValue *next = xi_binary(function, blocks[block], XI_ADD,
                                      &stub_int, current, increment);
            REQUIRE(increment != NULL && next != NULL);
            current = next;
        }
        if (block + 1u < block_count)
            xi_block_set_jump(blocks[block], blocks[block + 1u]);
        else
            xi_block_set_return(blocks[block], current);
    }
    function->stage = XI_STAGE_OPTIMIZED;

    XrSemanticPlan *semantic = NULL;
    char diagnostic[512] = {0};
    bool built = xr_semantic_plan_build(function, &semantic, diagnostic,
                                        sizeof(diagnostic));
    if (!built)
        fprintf(stderr, "resource ladder semantic build failed: %s\n",
                diagnostic);
    REQUIRE(built && semantic != NULL);
    REQUIRE(xr_semantic_plan_block_count(semantic) == block_count);
    REQUIRE(xr_semantic_plan_edge_count(semantic) == block_count - 1u);
    xr_free(blocks);
    xi_func_free(function);
    return semantic;
}

static ArtifactFixture make_fixture(uint32_t block_count,
                                    uint32_t operations_per_block) {
    ArtifactFixture fixture = {0};
    fixture.semantic =
        build_ladder_semantic(block_count, operations_per_block);
    fixture.profile = build_native_profile();
    char diagnostic[512] = {0};
    bool built = xr_target_plan_build(
        fixture.semantic, fixture.profile, &fixture.plan, diagnostic,
        sizeof(diagnostic));
    if (!built)
        fprintf(stderr, "resource ladder target build failed: %s\n",
                diagnostic);
    REQUIRE(built && xr_target_plan_is_verified(fixture.plan));
    REQUIRE(xr_xtp_encode_plan(fixture.plan, &fixture.bytes, &fixture.size,
                               diagnostic, sizeof(diagnostic)));
    REQUIRE(fixture.bytes != NULL && fixture.size >= XR_XTP_HEADER_SIZE);
    return fixture;
}

static void dispose_fixture(ArtifactFixture *fixture) {
    xr_xtp_encoded_free(fixture->bytes);
    xr_target_plan_free(fixture->plan);
    xr_target_profile_free(fixture->profile);
    xr_semantic_plan_free(fixture->semantic);
    memset(fixture, 0, sizeof(*fixture));
}

static uint8_t *copy_artifact(const ArtifactFixture *fixture) {
    uint8_t *copy = (uint8_t *) xr_malloc(fixture->size);
    REQUIRE(copy != NULL);
    memcpy(copy, fixture->bytes, fixture->size);
    return copy;
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
    xr_sha256_update(&context,
                     bytes + XR_XTP_FULL_DIGEST_OFFSET + sizeof(zero),
                     size - XR_XTP_FULL_DIGEST_OFFSET - sizeof(zero));
    xr_sha256_final(&context, bytes + XR_XTP_FULL_DIGEST_OFFSET);
}

static void resign_section(uint8_t *bytes, XrXtpSectionKind kind) {
    uint8_t *entry = directory_entry(bytes, kind);
    size_t offset = (size_t) xr_xtp_take_u64(entry + 8);
    size_t length = (size_t) xr_xtp_take_u64(entry + 16);
    xr_sha256(bytes + offset, length, entry + 40);
}

static void expect_deterministic_decode_failure(const uint8_t *bytes,
                                                size_t size,
                                                const char *code) {
    char first[512] = {0};
    for (uint32_t attempt = 0; attempt < 2; attempt++) {
        XrXtpCandidate *candidate = (XrXtpCandidate *) (uintptr_t) 1;
        char diagnostic[512] = {0};
        REQUIRE(!xr_xtp_decode_candidate(bytes, size, &candidate, diagnostic,
                                          sizeof(diagnostic)));
        REQUIRE(candidate == NULL && strstr(diagnostic, code) == diagnostic);
        if (attempt == 0)
            memcpy(first, diagnostic, sizeof(first));
        else
            REQUIRE(strcmp(first, diagnostic) == 0);
    }
}

static void expect_deterministic_materialize_failure(
    const ArtifactFixture *fixture, const uint8_t *bytes) {
    char first[512] = {0};
    for (uint32_t attempt = 0; attempt < 2; attempt++) {
        XrXtpCandidate *candidate = NULL;
        XrTargetPlan *plan = (XrTargetPlan *) (uintptr_t) 1;
        char diagnostic[512] = {0};
        REQUIRE(xr_xtp_decode_candidate(bytes, fixture->size, &candidate,
                                        diagnostic, sizeof(diagnostic)));
        REQUIRE(!xr_xtp_materialize_target_plan(
            candidate, fixture->semantic, fixture->profile, &plan, diagnostic,
            sizeof(diagnostic)));
        REQUIRE(plan == NULL && strstr(diagnostic, "XR_") == diagnostic);
        if (attempt == 0)
            memcpy(first, diagnostic, sizeof(first));
        else
            REQUIRE(strcmp(first, diagnostic) == 0);
        xr_xtp_candidate_release(candidate);
    }
}

static uint32_t count_nonempty_sections(const XrXtpCandidate *candidate) {
    uint32_t count = 0;
    for (uint32_t kind = 1; kind < XR_XTP_SECTION_COUNT; kind++) {
        const XrXtpSectionView *section = xr_xtp_candidate_section(
            candidate, (XrXtpSectionKind) kind);
        REQUIRE(section != NULL);
        count += section->count != 0;
    }
    return count;
}

static void test_valid_resource_ladder(void) {
    static const struct {
        uint32_t blocks;
        uint32_t operations_per_block;
    } ladder[] = {{1, 1}, {4, 4}, {16, 8}, {64, 16}};
    uint64_t previous_rows = 0;
    uint64_t previous_table_bytes = 0;
    size_t previous_artifact_bytes = 0;
    for (size_t index = 0; index < sizeof(ladder) / sizeof(ladder[0]);
         index++) {
        ArtifactFixture fixture =
            make_fixture(ladder[index].blocks,
                         ladder[index].operations_per_block);
        XrXtpCandidate *candidate = NULL;
        XrTargetPlan *decoded = NULL;
        char diagnostic[512] = {0};
        uint64_t start = xr_time_monotonic_ns();
        REQUIRE(xr_xtp_decode_candidate(fixture.bytes, fixture.size,
                                        &candidate, diagnostic,
                                        sizeof(diagnostic)));
        XrXtpResourceManifest resources = {0};
        REQUIRE(xr_xtp_candidate_resources(candidate, &resources));
        const XrXtpSectionView *instructions = xr_xtp_candidate_section(
            candidate, XR_XTP_SECTION_INSTRUCTIONS);
        REQUIRE(instructions != NULL && instructions->count > 0 &&
                instructions->flags == XR_XTP_SECTION_FLAG_COMPACT &&
                instructions->row_size == 0 &&
                instructions->length <
                    (size_t) instructions->count *
                        xr_xtp_wire_row_size(XR_XTP_SECTION_INSTRUCTIONS));
        REQUIRE(xr_xtp_materialize_target_plan(
            candidate, fixture.semantic, fixture.profile, &decoded,
            diagnostic, sizeof(diagnostic)));
        uint64_t elapsed = xr_time_monotonic_ns() - start;
        uint64_t peak = current_process_peak_bytes();
        REQUIRE(xr_target_plan_is_verified(decoded));
        REQUIRE(xr_fingerprint_equal(xr_target_plan_fingerprint(decoded),
                                     xr_target_plan_fingerprint(fixture.plan)));
        REQUIRE(resources.total_rows > previous_rows);
        REQUIRE(resources.table_bytes > previous_table_bytes);
        REQUIRE(fixture.size > previous_artifact_bytes);
        REQUIRE(resources.verification_work_units > resources.total_rows);
        REQUIRE(resources.verification_work_units ==
                resources.total_rows + instructions->length +
                    instructions->count);
        REQUIRE(resources.total_rows <= XR_XTP_MAX_TOTAL_ROWS);
        REQUIRE(resources.table_bytes <= XR_XTP_MAX_TABLE_BYTES);
        REQUIRE(resources.total_frame_bytes <= XR_XTP_MAX_TOTAL_FRAME_BYTES);
        REQUIRE(fixture.size <= XR_XTP_MAX_ARTIFACT_SIZE);
        REQUIRE(elapsed > 0 && elapsed <= XTP_LADDER_MAX_WALL_NS);
        REQUIRE(peak > 0 && peak <= XR_XTP_MAX_RUNTIME_LOAD_PEAK_BYTES);
        printf("XTP resource ladder: blocks=%u rows=%llu sections=%u "
               "artifact=%zu table=%llu work=%llu wall-ms=%.3f "
               "peak-bytes=%llu\n",
               ladder[index].blocks,
               (unsigned long long) resources.total_rows,
               count_nonempty_sections(candidate), fixture.size,
               (unsigned long long) resources.table_bytes,
               (unsigned long long) resources.verification_work_units,
               (double) elapsed / 1000000.0,
               (unsigned long long) peak);
        previous_rows = resources.total_rows;
        previous_table_bytes = resources.table_bytes;
        previous_artifact_bytes = fixture.size;
        xr_target_plan_free(decoded);
        xr_xtp_candidate_release(candidate);
        dispose_fixture(&fixture);
    }
}

static void test_exact_hard_budget_failures(void) {
    ArtifactFixture fixture = make_fixture(2, 2);
    static const struct {
        size_t offset;
        uint64_t value;
    } manifest_mutations[] = {
        {296, XR_XTP_MAX_TOTAL_ROWS + 1u},
        {304, XR_XTP_MAX_TABLE_BYTES + 1u},
        {312, XR_XTP_MAX_TOTAL_FRAME_BYTES + 1u},
        {320, XR_XTP_MAX_VERIFY_WORK_UNITS + 1u},
    };
    for (size_t i = 0;
         i < sizeof(manifest_mutations) / sizeof(manifest_mutations[0]);
         i++) {
        uint8_t *copy = copy_artifact(&fixture);
        xr_xtp_put_u64(copy + manifest_mutations[i].offset,
                       manifest_mutations[i].value);
        resign_artifact(copy, fixture.size);
        expect_deterministic_decode_failure(copy, fixture.size,
                                            "XR_EXEC_5003");
        xr_free(copy);
    }

    uint8_t *copy = copy_artifact(&fixture);
    uint8_t *functions =
        directory_entry(copy, XR_XTP_SECTION_FUNCTIONS);
    xr_xtp_put_u64(functions + 24,
                   xr_xtp_table_count_limit(XR_XTP_SECTION_FUNCTIONS) + 1u);
    resign_artifact(copy, fixture.size);
    expect_deterministic_decode_failure(copy, fixture.size,
                                        "XR_ARTIFACT_2003");
    xr_free(copy);

    expect_deterministic_decode_failure(
        fixture.bytes, XR_XTP_MAX_ARTIFACT_SIZE + 1u, "XR_EXEC_5003");
    REQUIRE(xr_xtp_runtime_peak_within_budget(
        XR_XTP_MAX_ARTIFACT_SIZE, XR_XTP_MAX_DECODED_TABLE_BYTES));
    REQUIRE(!xr_xtp_runtime_peak_within_budget(
        XR_XTP_MAX_ARTIFACT_SIZE,
        XR_XTP_MAX_DECODED_TABLE_BYTES + 1u));
    dispose_fixture(&fixture);
}

static void test_nonempty_extent_and_debug_field_mutations(void) {
    ArtifactFixture fixture = make_fixture(2, 2);
    uint32_t count = 0;
    REQUIRE(xr_target_plan_extents(fixture.plan, &count) != NULL && count > 0);
    static const size_t extent_field_offsets[] = {0, 4, 5, 6, 8, 12, 16, 20};
    for (size_t i = 0;
         i < sizeof(extent_field_offsets) / sizeof(extent_field_offsets[0]);
         i++) {
        uint8_t *copy = copy_artifact(&fixture);
        uint8_t *entry = directory_entry(copy, XR_XTP_SECTION_EXTENTS);
        size_t offset = (size_t) xr_xtp_take_u64(entry + 8);
        REQUIRE(xr_xtp_take_u64(entry + 24) > 0);
        copy[offset + extent_field_offsets[i]] ^= 1u;
        resign_section(copy, XR_XTP_SECTION_EXTENTS);
        resign_artifact(copy, fixture.size);
        expect_deterministic_materialize_failure(&fixture, copy);
        xr_free(copy);
    }

    REQUIRE(xr_target_plan_slots(fixture.plan, &count) != NULL && count > 0);
    uint8_t *copy = copy_artifact(&fixture);
    uint8_t *slots = directory_entry(copy, XR_XTP_SECTION_SLOTS);
    size_t slot_offset = (size_t) xr_xtp_take_u64(slots + 8);
    copy[slot_offset + 54] ^= 1u;
    resign_section(copy, XR_XTP_SECTION_SLOTS);
    resign_artifact(copy, fixture.size);
    expect_deterministic_materialize_failure(&fixture, copy);
    xr_free(copy);

    REQUIRE(xr_target_plan_root_maps(fixture.plan, &count) == NULL && count == 0);
    REQUIRE(xr_target_plan_root_slots(fixture.plan, &count) == NULL && count == 0);
    REQUIRE(xr_target_plan_cleanups(fixture.plan, &count) == NULL && count == 0);
    REQUIRE(xr_target_plan_entry_expectations(fixture.plan, &count) == NULL &&
            count == 0);
    REQUIRE(xr_target_plan_debug_facts(fixture.plan, &count) != NULL && count > 0);
    copy = copy_artifact(&fixture);
    uint8_t *debug_facts = directory_entry(copy, XR_XTP_SECTION_DEBUG_FACTS);
    size_t debug_offset = (size_t) xr_xtp_take_u64(debug_facts + 8);
    copy[debug_offset + 20] ^= 1u; /* semantic_operation */
    resign_section(copy, XR_XTP_SECTION_DEBUG_FACTS);
    resign_artifact(copy, fixture.size);
    expect_deterministic_materialize_failure(&fixture, copy);
    xr_free(copy);
    REQUIRE(XR_XTP_SECTION_ENTRY_EXPECTATIONS == XR_XTP_SECTION_COROUTINES + 1u);
    REQUIRE(XR_XTP_SECTION_DEBUG_FACTS == XR_XTP_SECTION_ENTRY_EXPECTATIONS + 1u);
    REQUIRE(XR_XTP_SECTION_MODULE_PARTITIONS == XR_XTP_SECTION_DEBUG_FACTS + 1u);
    REQUIRE(XR_XTP_SECTION_PROGRAM_GRAPHS == XR_XTP_SECTION_MODULE_PARTITIONS + 1u);
    REQUIRE(XR_XTP_SECTION_COUNT == XR_XTP_SECTION_PROGRAM_GRAPHS + 1u);
    puts("XTP mutation boundary: debug facts are source-backed and verified");
    dispose_fixture(&fixture);
}

static void *concurrent_load_thread(void *opaque) {
    enum { ITERATIONS = 16 };
    ConcurrentLoadContext *context = (ConcurrentLoadContext *) opaque;
    for (uint32_t iteration = 0; iteration < ITERATIONS; iteration++) {
        XrXtpCandidate *candidate = NULL;
        char diagnostic[512] = {0};
        if ((iteration & 1u) == 0) {
            if (!xr_xtp_decode_candidate(
                    context->fixture->bytes, context->fixture->size,
                    &candidate, diagnostic, sizeof(diagnostic)))
                return NULL;
        } else {
            candidate = xr_xtp_candidate_retain(context->shared_candidate);
            if (!candidate)
                return NULL;
        }
        XrTargetPlan *plan = NULL;
        bool valid = xr_xtp_materialize_target_plan(
                         candidate, context->fixture->semantic,
                         context->fixture->profile, &plan, diagnostic,
                         sizeof(diagnostic)) &&
                     plan && xr_target_plan_is_verified(plan) &&
                     xr_fingerprint_equal(xr_target_plan_fingerprint(plan),
                                          context->expected_fingerprint) &&
                     xr_module_generation_pin_acquire(
                         context->generation, XR_MODULE_GENERATION_PIN,
                         diagnostic, sizeof(diagnostic)) &&
                     xr_module_generation_pin_release(
                         context->generation, XR_MODULE_GENERATION_PIN,
                         diagnostic, sizeof(diagnostic));
        xr_target_plan_free(plan);
        xr_xtp_candidate_release(candidate);
        if (!valid)
            return NULL;
    }
    context->completed = true;
    return NULL;
}

static void test_concurrent_decode_materialize_and_generation_pins(void) {
    enum { THREAD_COUNT = 8, ITERATIONS = 16 };
    ArtifactFixture fixture = make_fixture(4, 4);
    XrXtpCandidate *shared_candidate = NULL;
    XrTargetPlan *decoded = NULL;
    char diagnostic[512] = {0};
    REQUIRE(xr_xtp_decode_candidate(fixture.bytes, fixture.size,
                                    &shared_candidate, diagnostic,
                                    sizeof(diagnostic)));
    REQUIRE(xr_xtp_materialize_target_plan(
        shared_candidate, fixture.semantic, fixture.profile, &decoded,
        diagnostic, sizeof(diagnostic)));

    XrRuntimeGenerationBudget budget = {
        .schema_version = XR_RUNTIME_GENERATION_SCHEMA_VERSION,
        .max_loaded_generations = 1,
        .max_total_pins = THREAD_COUNT,
        .max_pins_per_generation = THREAD_COUNT,
        .max_pins_by_kind = {THREAD_COUNT, THREAD_COUNT, THREAD_COUNT,
                             THREAD_COUNT, THREAD_COUNT},
    };
    XrRuntimeGenerationAuthority *authority = NULL;
    XrLoadedModuleGeneration *generation = NULL;
    REQUIRE(xr_runtime_generation_authority_create(
        &budget, &authority, diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_module_generation_load_verified_target_plan(
        authority, decoded, &generation, diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_module_generation_prepare(generation, diagnostic,
                                         sizeof(diagnostic)));
    REQUIRE(xr_module_generation_activate(generation, diagnostic,
                                          sizeof(diagnostic)));
    XrModuleGenerationSnapshot before = {0};
    REQUIRE(xr_module_generation_snapshot(generation, &before));

    ConcurrentLoadContext contexts[THREAD_COUNT] = {0};
    xr_thread_t threads[THREAD_COUNT] = {0};
    for (uint32_t i = 0; i < THREAD_COUNT; i++) {
        contexts[i] = (ConcurrentLoadContext) {
            .fixture = &fixture,
            .shared_candidate = shared_candidate,
            .generation = generation,
            .expected_fingerprint = xr_target_plan_fingerprint(decoded),
        };
        REQUIRE(xr_thread_create(&threads[i], concurrent_load_thread,
                                 &contexts[i]));
    }
    for (uint32_t i = 0; i < THREAD_COUNT; i++) {
        REQUIRE(xr_thread_join(threads[i], NULL) == 0);
        REQUIRE(contexts[i].completed);
    }

    XrModuleGenerationSnapshot after = {0};
    REQUIRE(xr_module_generation_snapshot(generation, &after));
    REQUIRE(after.state == XR_MODULE_GENERATION_ACTIVE);
    REQUIRE(after.total_pins == 0);
    REQUIRE(after.pins_by_kind[XR_MODULE_GENERATION_PIN] == 0);
    REQUIRE(after.revision ==
            before.revision + UINT64_C(2) * THREAD_COUNT * ITERATIONS);
    REQUIRE(memcmp(before.identity.generation_fingerprint,
                   after.identity.generation_fingerprint,
                   sizeof(before.identity.generation_fingerprint)) == 0);
    REQUIRE(xr_module_generation_verify(generation, diagnostic,
                                        sizeof(diagnostic)));
    REQUIRE(xr_module_generation_begin_drain(generation, diagnostic,
                                             sizeof(diagnostic)));
    REQUIRE(xr_module_generation_retire(generation, diagnostic,
                                        sizeof(diagnostic)));
    REQUIRE(xr_module_generation_unload(&generation, diagnostic,
                                        sizeof(diagnostic)));
    REQUIRE(xr_runtime_generation_authority_destroy(
        &authority, diagnostic, sizeof(diagnostic)));
    xr_target_plan_free(decoded);
    xr_xtp_candidate_release(shared_candidate);
    dispose_fixture(&fixture);
}

static void require_registration_surfaces_empty(
    XrRuntimeGenerationAuthority *authority, XrEntryCell *cell) {
    xr_mutex_lock(&authority->gate);
    REQUIRE(authority->live_generations == 0 && authority->total_pins == 0);
    xr_mutex_unlock(&authority->gate);
    xr_mutex_lock(&cell->gate);
    REQUIRE(cell->binding.generation == NULL && cell->binding.plan == NULL &&
            cell->binding.native_entry == NULL);
    xr_mutex_unlock(&cell->gate);
}

static void test_preverification_registration_boundary(void) {
    ArtifactFixture fixture = make_fixture(2, 2);
    XrRuntimeGenerationBudget budget = {
        .schema_version = XR_RUNTIME_GENERATION_SCHEMA_VERSION,
        .max_loaded_generations = 1,
        .max_total_pins = 4,
        .max_pins_per_generation = 4,
        .max_pins_by_kind = {4, 4, 4, 4, 4},
    };
    XrRuntimeGenerationAuthority *authority = NULL;
    XrEntryCell cell;
    char diagnostic[512] = {0};
    REQUIRE(xr_runtime_generation_authority_create(
        &budget, &authority, diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_entry_cell_init(&cell));
    require_registration_surfaces_empty(authority, &cell);

    uint8_t *copy = copy_artifact(&fixture);
    copy[XR_XTP_FULL_DIGEST_OFFSET] ^= 1u;
    expect_deterministic_decode_failure(copy, fixture.size,
                                        "XR_ARTIFACT_2002");
    require_registration_surfaces_empty(authority, &cell);
    xr_free(copy);

    copy = copy_artifact(&fixture);
    uint8_t *extents = directory_entry(copy, XR_XTP_SECTION_EXTENTS);
    size_t extent_offset = (size_t) xr_xtp_take_u64(extents + 8);
    copy[extent_offset + 5] ^= 1u;
    resign_section(copy, XR_XTP_SECTION_EXTENTS);
    resign_artifact(copy, fixture.size);
    expect_deterministic_materialize_failure(&fixture, copy);
    require_registration_surfaces_empty(authority, &cell);
    xr_free(copy);

    /* Decode and materialize expose no provider or finalizer callback handle.
     * The generation authority and canonical entry cell are the complete
     * observable registration surfaces available here, so the test keeps them
     * empty rather than inventing hooks that the runtime does not own. */
    REQUIRE(xr_entry_cell_dispose(&cell, diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_runtime_generation_authority_destroy(
        &authority, diagnostic, sizeof(diagnostic)));
    dispose_fixture(&fixture);
}

int main(void) {
    test_valid_resource_ladder();
    test_exact_hard_budget_failures();
    test_nonempty_extent_and_debug_field_mutations();
    test_concurrent_decode_materialize_and_generation_pins();
    test_preverification_registration_boundary();
    puts("XTP resource stress tests passed");
    return 0;
}
