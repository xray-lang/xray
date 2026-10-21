/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_lower_allocations.c - Representative unpublished lowering failures
 */
#include "lir_source_fixture.h"
#include "lir/xlir_lower.h"
#include "lir/xlir_verify.h"
#include "base/xchecks.h"

static size_t attempts, fail_at = SIZE_MAX;

static void *injected_allocate(size_t bytes) {
    if (attempts++ == fail_at)
        return NULL;
    return xr_malloc(bytes);
}

#undef xr_malloc
#define xr_malloc(bytes) injected_allocate(bytes)
#include "lir/xlir_lower.c"

static XrLirStatus run(const SourceFixture *fixture) {
    XrLirLowerRequest request = {fixture->closed, fixture->source.snapshot, false};
    XrLirLowered output = {0};
    XrLirLowerFailure failure = {0};
    XrLirStatus status = xr_lir_lower_checked(&request, &output, &failure);
    if (status == XR_LIR_OK) {
        CHECK(output.module && output.entry.id);
        OK(xr_lir_verify(output.module, NULL));
    } else {
        CHECK(!output.module && !output.entry.id);
        CHECK(failure.diagnostic.status == status && failure.diagnostic.message[0]);
    }
    xr_lir_lowered_free(&output);
    xr_lir_lower_failure_free(&failure);
    CHECK(!output.module && !failure.owned_path);
    return status;
}

static void diagnostic_copy_failure(void) {
    SourceFixture fixture =
        source_fixture("fn answer()->i64{return len(\"unsupported\")}\nprint(answer())\n");
    XrLirLowerRequest request = {fixture.closed, fixture.source.snapshot, false};
    XrLirLowered output = {0};
    XrLirLowerFailure failure = {0};
    attempts = 0;
    fail_at = SIZE_MAX;
    CHECK(xr_lir_lower_checked(&request, &output, &failure) == XR_LIR_UNSUPPORTED);
    CHECK(!output.module && failure.owned_path && failure.diagnostic.source.path == failure.owned_path);
    CHECK(attempts > 0);
    size_t path_point = attempts - 1;
    xr_lir_lower_failure_free(&failure);
    attempts = 0;
    fail_at = path_point;
    CHECK(xr_lir_lower_checked(&request, &output, &failure) == XR_LIR_OUT_OF_MEMORY);
    CHECK(!output.module && !output.entry.id && failure.diagnostic.status == XR_LIR_OUT_OF_MEMORY);
    CHECK(!failure.owned_path && !failure.diagnostic.source.path);
    xr_lir_lower_failure_free(&failure);
    source_fixture_free(&fixture);
    fail_at = SIZE_MAX;
}

int main(void) {
    SourceFixture fixture =
        source_fixture("fn add(n:i64)->i64{var i=0\nvar sum=0\nwhile(i<n){"
                       "sum=sum+(i<2 ? i : i+1)\ni=i+1}\nreturn sum}\nprint(add(5))\n");
    OK(run(&fixture));
    size_t total = attempts;
    CHECK(total > 0);
    const size_t points[] = {0, total / 2};
    for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); ++i) {
        attempts = 0;
        fail_at = points[i];
        CHECK(run(&fixture) == XR_LIR_OUT_OF_MEMORY);
        fail_at = SIZE_MAX;
        OK(run(&fixture));
    }
    source_fixture_free(&fixture);
    diagnostic_copy_failure();
    puts("representative lowering workspace and diagnostic-copy failures passed");
    return 0;
}
