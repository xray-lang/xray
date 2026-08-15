/*
 * benchmark.c - Direct typed TargetPlan scalar and frame-memory probe
 */

#include "../../../../src/base/xmalloc.h"
#include "../../../../src/ir/xi.h"
#include "../../../../src/plan/semantic/xr_semantic_builder.h"
#include "../../../../src/plan/target/xr_target_builder.h"
#include "../../../../src/runtime/value/xtype.h"
#include "../../../../src/vm/xr_typed_dispatch.h"
#include "../../../../src/vm/xr_typed_frame.h"
#include "../../../unit/plan/target_profile_test_fixture.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#define MAX_SAMPLES 64u

#ifndef XRAY_BUILD_COMMIT
#define XRAY_BUILD_COMMIT "unknown"
#endif
#ifndef XRAY_BUILD_DIRTY
#define XRAY_BUILD_DIRTY 1
#endif
#ifndef XRAY_BUILD_PROFILE
#define XRAY_BUILD_PROFILE "unknown"
#endif

#if XR_TYPED_FRAME_HAS_SLOT_STATE_METADATA
#error "The typed target VM performance probe requires zero Release slot metadata"
#endif

typedef struct BenchmarkFixture {
    XrSemanticPlan *semantic;
    XrTargetProfile *profile;
    XrTargetPlan *plan;
    XrFingerprint fingerprint;
    uint32_t arithmetic_operations;
} BenchmarkFixture;

static XrType benchmark_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static volatile uint64_t benchmark_sink;

static void fail(const char *message) {
    fprintf(stderr, "typed target VM benchmark: %s\n", message);
    exit(2);
}

static uint32_t parse_count(const char *text) {
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || !end || *end || value == 0 || value > UINT32_MAX)
        fail("invalid positive count argument");
    return (uint32_t) value;
}

static uint64_t now_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    uint64_t whole = (uint64_t) (counter.QuadPart / frequency.QuadPart);
    uint64_t fraction = (uint64_t) (counter.QuadPart % frequency.QuadPart);
    return whole * UINT64_C(1000000000) +
           fraction * UINT64_C(1000000000) / (uint64_t) frequency.QuadPart;
#else
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t) value.tv_sec * UINT64_C(1000000000) +
           (uint64_t) value.tv_nsec;
#endif
}

static BenchmarkFixture build_fixture(uint32_t arithmetic_operations) {
    BenchmarkFixture fixture = {.arithmetic_operations = arithmetic_operations};
    XiFunc *function = xi_func_new("typed_vm_scalar_benchmark", &benchmark_int);
    if (!function)
        fail("cannot allocate benchmark function");
    function->nparams = 2;
    function->min_params = 2;
    function->params = (XiValue **) xr_calloc(2, sizeof(*function->params));
    XiBlock *entry = xi_block_new(function);
    if (!function->params || !entry)
        fail("cannot allocate benchmark parameters");
    entry->sealed = true;
    function->params[0] = xi_param(function, entry, 0, &benchmark_int);
    function->params[1] = xi_param(function, entry, 1, &benchmark_int);
    XiValue *value = function->params[0];
    if (!value || !function->params[1])
        fail("cannot create benchmark parameters");
    for (uint32_t i = 0; i < arithmetic_operations; i++) {
        XiValue *next = xi_value_new(function, entry, XI_ADD, &benchmark_int, 2);
        if (!next)
            fail("cannot create benchmark arithmetic row");
        next->args[0] = value;
        next->args[1] = function->params[1];
        value = next;
    }
    xi_block_set_return(entry, value);
    function->stage = XI_STAGE_OPTIMIZED;
    char error[512] = {0};
    if (!xr_semantic_plan_build(function, &fixture.semantic, error,
                                sizeof(error)))
        fail(error);
    xi_func_free(function);
    fixture.profile = xr_test_target_profile_build(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    if (!fixture.profile ||
        !xr_target_plan_build(fixture.semantic, fixture.profile, &fixture.plan,
                              error, sizeof(error)))
        fail(error[0] ? error : "cannot build benchmark TargetPlan");
    if (xr_target_plan_function_execution_family_mask(fixture.plan, 0) !=
        XR_TARGET_EXECUTION_SCALAR_I64_CLOSED)
        fail("benchmark TargetPlan is not executable by the typed scalar VM");
    fixture.fingerprint = xr_target_plan_fingerprint(fixture.plan);
    return fixture;
}

static void dispose_fixture(BenchmarkFixture *fixture) {
    xr_target_plan_free(fixture->plan);
    xr_target_profile_free(fixture->profile);
    xr_semantic_plan_free(fixture->semantic);
    memset(fixture, 0, sizeof(*fixture));
}

static uint64_t measure_scalar(const BenchmarkFixture *fixture,
                               uint32_t executions) {
    const int64_t arguments[2] = {7, 3};
    int64_t expected = 7 + (int64_t) fixture->arithmetic_operations * 3;
    uint64_t checksum = 0;
    uint64_t start = now_ns();
    for (uint32_t i = 0; i < executions; i++) {
        int64_t result = 0;
        if (xr_typed_dispatch_execute_i64(
                fixture->plan, &fixture->fingerprint, 0, arguments, 2,
                &result) != XR_TYPED_DISPATCH_OK ||
            result != expected)
            fail("typed scalar execution produced the wrong result");
        checksum += (uint64_t) result;
    }
    uint64_t elapsed = now_ns() - start;
    benchmark_sink ^= checksum;
    return elapsed;
}

static uint64_t measure_frames(const BenchmarkFixture *fixture,
                               const XrTypedFrameLimits *limits,
                               uint32_t frames) {
    uint64_t checksum = 0;
    uint64_t start = now_ns();
    for (uint32_t i = 0; i < frames; i++) {
        XrTypedFrame *frame = NULL;
        if (xr_typed_frame_create(fixture->plan, &fixture->fingerprint, 0,
                                  limits, &frame) != XR_TYPED_FRAME_OK)
            fail("typed frame creation failed");
        checksum += xr_typed_frame_arena_size(frame);
        xr_typed_frame_free(frame);
    }
    uint64_t elapsed = now_ns() - start;
    benchmark_sink ^= checksum;
    return elapsed;
}

static XrTypedFrameMemoryFootprint measure_footprint(
    const BenchmarkFixture *fixture, const XrTypedFrameLimits *limits,
    uint32_t *slot_count, uint32_t *plan_frame_bytes,
    uint16_t *frame_alignment) {
    XrTypedFrame *frame = NULL;
    if (xr_typed_frame_create(fixture->plan, &fixture->fingerprint, 0, limits,
                              &frame) != XR_TYPED_FRAME_OK)
        fail("cannot create footprint probe frame");
    XrTypedFrameMemoryFootprint footprint;
    if (xr_typed_frame_memory_footprint(frame, &footprint) !=
        XR_TYPED_FRAME_OK)
        fail("cannot describe footprint probe frame");
    *slot_count = xr_typed_frame_slot_count(frame);
    uint32_t function_count = 0;
    const XrTargetFunctionRecord *functions =
        xr_target_plan_functions(fixture->plan, &function_count);
    if (!functions || function_count != 1)
        fail("benchmark TargetPlan function table is not exact");
    *plan_frame_bytes = functions[0].frame_size;
    *frame_alignment = functions[0].frame_align;
    xr_typed_frame_free(frame);
    return footprint;
}

static void print_samples(const uint64_t *samples, uint32_t count) {
    putchar('[');
    for (uint32_t i = 0; i < count; i++)
        printf("%s%" PRIu64, i ? "," : "", samples[i]);
    putchar(']');
}

int main(int argc, char **argv) {
    if (argc != 6)
        fail("expected: warmups samples arithmetic-ops scalar-executions frame-iterations");
    uint32_t warmups = parse_count(argv[1]);
    uint32_t samples = parse_count(argv[2]);
    uint32_t arithmetic_operations = parse_count(argv[3]);
    uint32_t scalar_executions = parse_count(argv[4]);
    uint32_t frame_iterations = parse_count(argv[5]);
    if (samples > MAX_SAMPLES)
        fail("sample count exceeds the benchmark bound");

    BenchmarkFixture fixture = build_fixture(arithmetic_operations);
    XrTypedFrameLimits limits;
    xr_typed_frame_limits_default(&limits);
    uint64_t scalar_ns[MAX_SAMPLES] = {0};
    uint64_t frame_ns[MAX_SAMPLES] = {0};
    for (uint32_t i = 0; i < warmups; i++) {
        (void) measure_scalar(&fixture, scalar_executions);
        (void) measure_frames(&fixture, &limits, frame_iterations);
    }
    for (uint32_t i = 0; i < samples; i++) {
        scalar_ns[i] = measure_scalar(&fixture, scalar_executions);
        frame_ns[i] = measure_frames(&fixture, &limits, frame_iterations);
    }
    uint32_t slot_count = 0;
    uint32_t plan_frame_bytes = 0;
    uint16_t frame_alignment = 0;
    XrTypedFrameMemoryFootprint footprint =
        measure_footprint(&fixture, &limits, &slot_count, &plan_frame_bytes,
                          &frame_alignment);

    printf("{\"schema\":1,\"build_commit\":\"%s\","
           "\"build_dirty\":%s,\"build_profile\":\"%s\","
           "\"release_build\":%s,"
           "\"slot_state_metadata_enabled\":%u,\"warmup_runs\":%u,"
           "\"sample_count\":%u,\"scalar\":{"
           "\"arithmetic_operations_per_execution\":%u,"
           "\"executions_per_sample\":%u,\"samples_ns\":",
#ifdef NDEBUG
           XRAY_BUILD_COMMIT, XRAY_BUILD_DIRTY ? "true" : "false",
           XRAY_BUILD_PROFILE,
           "true",
#else
           XRAY_BUILD_COMMIT, XRAY_BUILD_DIRTY ? "true" : "false",
           XRAY_BUILD_PROFILE,
           "false",
#endif
           XR_TYPED_FRAME_HAS_SLOT_STATE_METADATA, warmups, samples,
           arithmetic_operations, scalar_executions);
    print_samples(scalar_ns, samples);
    printf("},\"frame_memory\":{\"frames_per_sample\":%u,"
           "\"samples_ns\":",
           frame_iterations);
    print_samples(frame_ns, samples);
    printf("},\"footprint\":{\"fixed_frame_bytes\":%zu,"
           "\"arena_allocation_bytes\":%zu,"
           "\"alignment_padding_bytes\":%zu,"
           "\"slot_state_metadata_bytes\":%zu,\"total_bytes\":%zu,"
           "\"slot_count\":%u,\"plan_frame_bytes\":%u,"
           "\"frame_alignment\":%u,"
           "\"max_total_bytes\":%zu},\"checksum\":%" PRIu64 "}\n",
           footprint.fixed_frame_bytes, footprint.arena_allocation_bytes,
           footprint.alignment_padding_bytes,
           footprint.slot_state_metadata_bytes, footprint.total_bytes,
           slot_count, plan_frame_bytes, frame_alignment,
           limits.max_total_bytes,
           benchmark_sink);
    dispose_fixture(&fixture);
    return 0;
}
