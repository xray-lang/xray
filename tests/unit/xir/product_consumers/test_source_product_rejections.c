/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_rejections.c - Owned diagnostics from rejected source
 *
 * KEY CONCEPT:
 *   Semantic failure publishes no product and owns its path beyond parsing.
 */
#include "base/xmalloc.h"
#include "base/xplatform.h"
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "../xir_instance_compile_observer.h"

typedef struct RejectionCase {
    const char *name, *file, *message;
    XrXirStatus status;
    int column;
} RejectionCase;

typedef struct RejectionRun {
    XrXirStatus status;
    XrCompileResourceStats stats;
    size_t attempts;
    bool injected;
} RejectionRun;

static const RejectionCase rejection_cases[] = {
    {"bare_nullable", XR_REJECTION_ROOT "/bare_nullable/main.xr", "if requires bool", XR_XIR_BAD_TYPE, -1},
    {"missing_name", XR_REJECTION_ROOT "/missing_name/main.xr", "name is not an initialized value", XR_XIR_BAD_VALUE, 29}
};

static XrCompileResourceLimits limits(void) {
    return (XrCompileResourceLimits){UINT64_C(67108864), UINT64_C(16777216), UINT64_C(128000000)};
}

static bool same_path(const char *left, const char *right) {
    if (!left || !right) return false;
    while (*left && *right) {
        char a = *left++, b = *right++;
#if XR_OS_WINDOWS
        if (a == '\\') a = '/';
        if (b == '\\') b = '/';
#endif
        if (a != b) return false;
    }
    return !*left && !*right;
}

static void diagnostic(const RejectionCase *test, const XrXirSourceProductDiagnostic *value) {
    CHECK(value->stage == XR_XIR_SOURCE_PRODUCT_CHECK && value->status == test->status);
    CHECK(value->source.status == test->status && value->source.module == 0);
    CHECK(value->source.line == 1);
    if (test->column >= 0) CHECK(value->source.column == test->column);
    CHECK(!strcmp(value->source.message, test->message));
    CHECK(same_path(value->source_path, test->file));
    CHECK(!value->snapshot);
}

static RejectionRun run(const RejectionCase *test, size_t failure, XrCompileResourceLimits budget) {
    instance_compile_zero();
    instance_compile_attempts = 0;
    instance_compile_fail_at = failure;
    instance_compile_injected = false;
    RejectionRun result = {XR_XIR_OUT_OF_MEMORY, {0}, 0, false};
    XrXirCompileContext context = {0};
    context.limits = xr_xir_compile_default_limits();
    XrCompilerSession *session = NULL;
    XrXirSourceProduct *product = NULL;
    XrXirSourceProductDiagnostic error = {0};
    XrCompileResourceStatus opened = xr_compile_resources_new(&budget, &context.resources);
    if (opened != XR_COMPILE_RESOURCE_OK) {
        result.status = opened == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
        goto done;
    }
    XrCompilerSessionStatus parsed = xr_compile_session_new(context.resources, &session);
    if (parsed != XR_COMPILER_SESSION_OK) {
        result.status = parsed == XR_COMPILER_SESSION_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
        goto release;
    }
    const XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_REJECTION_ROOT};
    const XrXirSourceProductRequest request = {
        {session, test->file, &authority, &context, NULL, NULL, XR_XIR_PROGRAM, NULL},
        {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
    result.status = xr_xir_compile_source_product_build(&request, &product, &error);
    CHECK(!product);
release:
    CHECK(xr_compile_resources_stats(context.resources, &result.stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(result.stats.live_bytes == instance_compile_bytes);
    xr_compile_session_free(session);
    if (failure == SIZE_MAX && result.status == test->status)
        diagnostic(test, &error);
    xr_compile_resources_release(context.resources);
    context.resources = NULL;
    if (failure == SIZE_MAX && result.status == test->status) {
        CHECK(instance_compile_live > 0 && instance_compile_bytes > 0);
        diagnostic(test, &error);
        printf("rejection case=%s stage=%u status=%u module=%u line=%d column=%d path=%s owned-after-ledger-drop=1\n",
            test->name, error.stage, error.status, error.source.module, error.source.line,
            error.source.column, error.source_path);
    }
    xr_xir_compile_source_product_diagnostic_free(&error);
    CHECK(!error.snapshot && !error.source_path && !error.source.message[0]);
    xr_xir_compile_source_product_diagnostic_free(&error);
done:
    result.attempts = instance_compile_attempts;
    result.injected = instance_compile_injected;
    instance_compile_fail_at = SIZE_MAX;
    instance_compile_zero();
    return result;
}

static void fault(const RejectionCase *test, size_t ordinal) {
    RejectionRun result = run(test, ordinal, limits());
    CHECK(result.status == XR_XIR_OUT_OF_MEMORY && result.injected && result.attempts > ordinal);
    printf("rejection ordinal=%zu physical=0/0\n", ordinal);
}

static void axes(const RejectionCase *test, const RejectionRun *baseline) {
    const uint64_t values[] = {baseline->stats.allocated_bytes, baseline->stats.peak_bytes, baseline->stats.work};
    for (unsigned axis = 0; axis < 3; ++axis) {
        CHECK(values[axis]);
        for (unsigned below = 0; below < 2; ++below) {
            XrCompileResourceLimits budget = limits();
            uint64_t *threshold = axis == 0 ? &budget.allocated_bytes : axis == 1 ? &budget.live_bytes : &budget.work;
            *threshold = values[axis] - below;
            RejectionRun result = run(test, SIZE_MAX, budget);
            CHECK(result.status == (below ? XR_XIR_BUDGET : test->status));
            CHECK(!result.injected);
            printf("rejection axis=%u below=%u limit=%llu status=%u physical=0/0\n",
                axis, below, (unsigned long long)*threshold, result.status);
        }
    }
}

int main(int argc, char **argv) {
    CHECK(argc >= 2 && argc <= 4);
    const RejectionCase *test = NULL;
    for (size_t i = 0; i < sizeof(rejection_cases) / sizeof(rejection_cases[0]); ++i)
        if (!strcmp(argv[1], rejection_cases[i].name)) test = &rejection_cases[i];
    CHECK(test);
    RejectionRun baseline = run(test, SIZE_MAX, limits());
    CHECK(baseline.status == test->status && baseline.attempts && !baseline.injected);
    printf("rejection-baseline case=%s sites=%zu allocated=%llu peak=%llu work=%llu physical=0/0\n",
        test->name, baseline.attempts, (unsigned long long)baseline.stats.allocated_bytes,
        (unsigned long long)baseline.stats.peak_bytes, (unsigned long long)baseline.stats.work);
    if (argc == 3 && !strcmp(argv[2], "--axes"))
        axes(test, &baseline);
    else if (argc == 3 && !strcmp(argv[2], "--compiler")) {
        for (size_t ordinal = 0; ordinal < baseline.attempts; ++ordinal) fault(test, ordinal);
        printf("rejection-summary case=%s sites=%zu covered=%zu physical=0/0\n",
            test->name, baseline.attempts, baseline.attempts);
    } else if (argc == 4 && !strcmp(argv[2], "--compiler-site")) {
        char *end = NULL;
        unsigned long long ordinal = strtoull(argv[3], &end, 10);
        CHECK(argv[3][0] && end && !*end && ordinal < baseline.attempts);
        fault(test, (size_t)ordinal);
    } else CHECK(argc == 2);
    instance_compile_report();
    return 0;
}
