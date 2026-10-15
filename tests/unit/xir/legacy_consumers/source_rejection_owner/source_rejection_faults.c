/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_rejection_faults.c - Complete actual compiler cuts through the SDK owner
 *
 * KEY CONCEPT:
 *   The real consumer main runs with the production compiler allocator observed.
 *   API wrappers record actual failures and never change their results.
 */
#include "api/xsource_compile.h"
#include "xir/xxir_constraint_proof.h"
#include "xir/xxir_types.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"

static bool observed_oom, observed_budget, override_limits;
static XrCompileResourceLimits supplied_limits;
static XrCompileResourceStats observed_stats;

static XrXirStatus observe_xir(XrXirStatus status) {
    observed_oom |= status == XR_XIR_OUT_OF_MEMORY;
    observed_budget |= status == XR_XIR_BUDGET;
    return status;
}

static XrCompileResourceLimits consumer_limits(void) {
    return override_limits ? supplied_limits : xr_source_compile_default_resource_limits();
}

static XrCompileResourceStatus observed_new(const XrCompileResourceLimits *limits, XrCompileResources **out) {
    XrCompileResources *before = *out;
    XrCompileResourceStatus status = xr_compile_resources_new(limits, out);
    CHECK(status == XR_COMPILE_RESOURCE_OK || *out == before);
    observed_oom |= status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY;
    observed_budget |= status == XR_COMPILE_RESOURCE_BUDGET;
    return status;
}

static XrCompileResourceStatus observed_statistics(const XrCompileResources *resources, XrCompileResourceStats *out) {
    XrCompileResourceStatus status = xr_compile_resources_stats(resources, out);
    if (status == XR_COMPILE_RESOURCE_OK) {
        CHECK(out->live_bytes == instance_compile_bytes);
        observed_stats = *out;
    }
    return status;
}

static XrSourceCompileStatus observed_build(const XrSourceCompileRequest *request,
    XrXirSourceProduct **out, XrSourceCompileDiagnostic *diagnostic) {
    XrXirSourceProduct *before = *out;
    XrSourceCompileStatus status = xr_source_compile_build(request, out, diagnostic);
    CHECK(status == XR_SOURCE_COMPILE_OK || *out == before);
    observed_oom |= status == XR_SOURCE_COMPILE_OUT_OF_MEMORY;
    observed_budget |= status == XR_SOURCE_COMPILE_BUDGET;
    return status;
}

static XrXirStatus observed_verify(const XrXirSourceProduct *product, size_t limit, XrXirDiagnostic *diagnostic) {
    return observe_xir(xr_xir_compile_source_product_verify(product, limit, diagnostic));
}

static XrXirStatus observed_admit(const XrXirSourceProduct *product, XrXirSourceDiagnostic *diagnostic) {
    return observe_xir(xr_xir_compile_source_product_public_admit(product, diagnostic));
}

static XrXirStatus observed_read(const XrXirCompileContext *context, const void *bytes,
    size_t length, XrXirArtifact **out, XrXirDiagnostic *diagnostic) {
    XrXirArtifact *before = *out;
    XrXirStatus status = xr_xir_compile_checked_read(context, bytes, length, out, diagnostic);
    CHECK(status == XR_XIR_OK || *out == before);
    return observe_xir(status);
}

static XrXirStatus observed_storage(const XrXirCompileContext *context,
    const XrXirProofContext *proof, XrXirType type) {
    return observe_xir(xr_xir_compile_type_storage_prove(context, proof, type));
}

static XrXirStatus observed_markers(const XrXirCompileContext *context,
    const XrXirProofContext *proof, XrXirType type, uint32_t markers) {
    return observe_xir(xr_xir_compile_type_markers_prove(context, proof, type, markers));
}

/* Only the consumer's repeated report is muted, not compiler calls or results. */
static int muted_consumer_report(const char *format, ...) { (void)format; return 0; }
#define xr_source_compile_default_resource_limits consumer_limits
#define xr_compile_resources_new observed_new
#define xr_compile_resources_stats observed_statistics
#define xr_source_compile_build observed_build
#define xr_xir_compile_source_product_verify observed_verify
#define xr_xir_compile_source_product_public_admit observed_admit
#define xr_xir_compile_checked_read observed_read
#define xr_xir_compile_type_storage_prove observed_storage
#define xr_xir_compile_type_markers_prove observed_markers
#define main actual_source_rejection_main
#define printf muted_consumer_report
#include "source_rejection_owner.c"
#undef printf
#undef main
#undef xr_source_compile_default_resource_limits
#undef xr_compile_resources_new
#undef xr_compile_resources_stats
#undef xr_source_compile_build
#undef xr_xir_compile_source_product_verify
#undef xr_xir_compile_source_product_public_admit
#undef xr_xir_compile_checked_read
#undef xr_xir_compile_type_storage_prove
#undef xr_xir_compile_type_markers_prove

typedef struct FaultRun {
    int code;
    size_t attempts;
    bool injected, oom, budget;
    XrCompileResourceStats stats;
} FaultRun;

static FaultRun run_owner(char **argv, size_t failure, const XrCompileResourceLimits *limits) {
    instance_compile_zero(); instance_compile_attempts = 0; instance_compile_peak = 0;
    instance_compile_fail_at = failure; instance_compile_injected = false;
    observed_oom = false; observed_budget = false; observed_stats = (XrCompileResourceStats){0};
    override_limits = limits != NULL;
    if (limits) supplied_limits = *limits;
    int code = actual_source_rejection_main(5, argv);
    FaultRun result = {code, instance_compile_attempts, instance_compile_injected,
        observed_oom, observed_budget, observed_stats};
    instance_compile_fail_at = SIZE_MAX; instance_compile_zero();
    return result;
}

static size_t number(const char *text) {
    char *end = NULL; unsigned long long value = strtoull(text, &end, 10);
    CHECK(text[0] >= '0' && text[0] <= '9' && end && !*end && value <= SIZE_MAX);
    return (size_t)value;
}

static void resource_axes(char **argv, const FaultRun *baseline) {
    uint64_t values[3] = {baseline->stats.allocated_bytes, baseline->stats.peak_bytes, baseline->stats.work};
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned below = 0; below < 2; ++below) {
        CHECK(values[axis] > 1);
        XrCompileResourceLimits limits = xr_source_compile_default_resource_limits();
        uint64_t *limit = axis == 0 ? &limits.allocated_bytes : axis == 1 ? &limits.live_bytes : &limits.work;
        *limit = values[axis] - below;
        FaultRun result = run_owner(argv, SIZE_MAX, &limits);
        printf("sdk-axis axis=%u below=%u limit=%llu code=%d budget=%u injected=%u physical=0/0\n",
            axis, below, (unsigned long long)*limit, result.code, (unsigned)result.budget, (unsigned)result.injected);
        CHECK(!result.injected && (below ? result.code != 0 && result.budget : result.code == 0 && !result.budget));
    }
}

int main(int argc, char **argv) {
    CHECK(argc == 6 || argc == 8);
    FaultRun baseline = run_owner(argv, SIZE_MAX, NULL);
    CHECK(baseline.code == 0 && baseline.attempts && !baseline.injected && !baseline.oom && !baseline.budget);
    printf("sdk-baseline sites=%zu allocated=%llu peak=%llu work=%llu physical=0/0\n", baseline.attempts,
        (unsigned long long)baseline.stats.allocated_bytes, (unsigned long long)baseline.stats.peak_bytes,
        (unsigned long long)baseline.stats.work);
    if (argc == 6 && !strcmp(argv[5], "--baseline")) return 0;
    if (argc == 6 && !strcmp(argv[5], "--axes")) { resource_axes(argv, &baseline); return 0; }
    CHECK(argc == 8 && !strcmp(argv[5], "--shard"));
    size_t shard = number(argv[6]), shards = number(argv[7]), covered = 0;
    CHECK(shards > 0 && shards <= 8 && shard < shards);
    for (size_t index = shard; index < baseline.attempts; index += shards) {
        FaultRun result = run_owner(argv, index, NULL);
        printf("sdk-fault ordinal=%zu code=%d oom=%u injected=%u attempts=%zu physical=0/0\n",
            index, result.code, (unsigned)result.oom, (unsigned)result.injected, result.attempts);
        CHECK(result.code != 0 && result.oom && !result.budget && result.injected && result.attempts > index);
        ++covered;
    }
    printf("sdk-summary sites=%zu shard=%zu shards=%zu covered=%zu physical=0/0\n",baseline.attempts,shard,shards,covered);
    return 0;
}
