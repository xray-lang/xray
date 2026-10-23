/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_lower_allocations.c - Observe lowering workspace and failure owners
 */
#include "lir_source_fixture.h"
#include "lir/xlir_lower.h"
#include "lir/xlir_verify.h"
#include "base/xchecks.h"

static void *allocations[512];
static size_t attempts, live, fail_at = SIZE_MAX;

static void *observed_allocate(size_t bytes) {
    if (attempts++ == fail_at)
        return NULL;
    void *memory = xr_malloc(bytes);
    CHECK(memory);
    size_t i = 0;
    while (i < 512 && allocations[i])
        ++i;
    CHECK(i < 512);
    allocations[i] = memory;
    ++live;
    return memory;
}
static void observed_free(void *memory) {
    if (!memory)
        return;
    size_t i = 0;
    while (i < 512 && allocations[i] != memory)
        ++i;
    CHECK(i < 512 && live);
    allocations[i] = NULL;
    --live;
    xr_free(memory);
}

/* Source and SSA-core allocations retain their own independent owners. This
 * observer injects every workspace allocation in the actual source adapter. */
#undef xr_malloc
#undef xr_free
#define xr_malloc(n) observed_allocate(n)
#define xr_free(p) observed_free(p)
#include "lir/xlir_lower.c"

static XrLirStatus run(const SourceFixture *fixture) {
    XrLirLowerRequest request = {fixture->closed, fixture->source.snapshot, false};
    XrLirLowered output = {0};
    XrLirLowerFailure failure = {0};
    XrLirStatus status = xr_lir_lower_program_checked(&request, &output, &failure);
    if (status == XR_LIR_OK) {
        CHECK(output.module && output.entry.id);
        OK(xr_lir_verify(output.module, NULL));
    } else {
        CHECK(!output.module && !output.entry.id);
        CHECK(failure.diagnostic.status == status && failure.diagnostic.message[0]);
    }
    xr_lir_lowered_free(&output);
    xr_lir_lower_failure_free(&failure);
    CHECK(live == 0);
    return status;
}

int main(void) {
    SourceFixture fixture =
        source_fixture("fn add(n:i64)->i64{var i=0\nvar sum=0\nwhile(i<n){"
                       "sum=sum+(i<2 ? i : i+1)\ni=i+1}\nreturn sum}\nprint(add(5))\n");
    OK(run(&fixture));
    size_t total = attempts;
    CHECK(total > 30);
    for (size_t i = 0; i < total; ++i) {
        attempts = 0;
        fail_at = i;
        CHECK(run(&fixture) == XR_LIR_OUT_OF_MEMORY);
    }
    fail_at = SIZE_MAX;
    attempts = 0;
    OK(run(&fixture));
    source_fixture_free(&fixture);

    fixture = source_fixture("fn answer()->i64{return len(\"unsupported\")}\nprint(answer())\n");
    attempts = 0;
    CHECK(run(&fixture) == XR_LIR_UNSUPPORTED);
    size_t rejected_attempts = attempts;
    CHECK(rejected_attempts > 0);
    fail_at = rejected_attempts - 1;
    attempts = 0;
    CHECK(run(&fixture) == XR_LIR_OUT_OF_MEMORY);
    source_fixture_free(&fixture);
    printf("all %zu lowering workspace allocations and failed diagnostic ownership passed\n",
           total);
    return 0;
}
