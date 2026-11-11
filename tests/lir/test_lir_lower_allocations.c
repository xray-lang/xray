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
typedef struct LowerBlock { void *pointer; size_t bytes; } LowerBlock;
static LowerBlock observed_lower_blocks[1024];
static size_t lower_live, lower_bytes;
static void injected_release(void *pointer) {
    if (pointer) {
        size_t i=0;
        while (i<lower_live && observed_lower_blocks[i].pointer != pointer) ++i;
        CHECK(i<lower_live);
        lower_bytes -= observed_lower_blocks[i].bytes;
        observed_lower_blocks[i] = observed_lower_blocks[--lower_live];
    }
    xr_free(pointer);
}

static void *injected_allocate(size_t bytes) {
    if (attempts++ == fail_at)
        return NULL;
    void *pointer = xr_malloc(bytes);
    if (pointer) {
        CHECK(lower_live<1024 && bytes<=SIZE_MAX-lower_bytes);
        observed_lower_blocks[lower_live++] = (LowerBlock){pointer,bytes}; lower_bytes += bytes;
    }
    return pointer;
}

#undef xr_malloc
#define xr_malloc(bytes) injected_allocate(bytes)
#undef xr_free
#define xr_free(pointer) injected_release(pointer)
#include "lir/xlir_lower.c"

static XrLirStatus run(const SourceFixture *fixture) {
    size_t prior_live=lower_live, prior_bytes=lower_bytes;
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
    CHECK(!output.module && !failure.owned_path);
    CHECK(lower_live==prior_live && lower_bytes==prior_bytes);
    return status;
}

static void supported_length(void) {
    SourceFixture fixture =
        source_fixture("fn answer()->i64{return len(\"unsupported\")}\nprint(answer())\n");
    attempts=0;fail_at=SIZE_MAX;OK(run(&fixture));
    size_t total=attempts;CHECK(total>1);
    const size_t points[]={0,total/2};
    for(size_t i=0;i<sizeof(points)/sizeof(points[0]);++i) {
        attempts=0;fail_at=points[i];CHECK(run(&fixture)==XR_LIR_OUT_OF_MEMORY);
        fail_at=SIZE_MAX;OK(run(&fixture));
    }
    source_fixture_free(&fixture);
}
static void constant_initializer_failures(void) {
    SourceFixture fixture = source_fixture(
        "const n=6*7\nconst f=(n as f64)/2.0\nconst b=f>20.0\nprint(n)\n");
    attempts = 0;
    fail_at = SIZE_MAX;
    OK(run(&fixture));
    size_t total = attempts;
    CHECK(total > 10);
    for (size_t point = 0; point < total; ++point) {
        attempts = 0;
        fail_at = point;
        CHECK(run(&fixture) == XR_LIR_OUT_OF_MEMORY);
    }
    fail_at = SIZE_MAX;
    OK(run(&fixture));
    source_fixture_free(&fixture);
}
static void diagnostic_copy_failure(void) {
    SourceFixture fixture = source_fixture(
        "struct Pair{x:i64}\nfn answer()->i64{const p=Pair{x:42}\nreturn p.x}\nprint(answer())\n");
    XrLirLowerRequest request = {fixture.closed, fixture.source.snapshot, false};
    XrLirLowered output = {0};
    XrLirLowerFailure failure = {0};
    attempts = 0;
    fail_at = SIZE_MAX;
    CHECK(xr_lir_lower_program_checked(&request, &output, &failure) == XR_LIR_UNSUPPORTED);
    CHECK(strstr(failure.diagnostic.message,"STRUCT_NEW"));
    CHECK(!output.module && failure.owned_path && failure.diagnostic.source.path == failure.owned_path);
    CHECK(attempts > 0);
    size_t path_point = attempts - 1;
    xr_lir_lower_failure_free(&failure);
    attempts = 0;
    fail_at = path_point;
    CHECK(xr_lir_lower_program_checked(&request, &output, &failure) == XR_LIR_OUT_OF_MEMORY);
    CHECK(!output.module && !output.entry.id && failure.diagnostic.status == XR_LIR_OUT_OF_MEMORY);
    CHECK(!failure.owned_path && !failure.diagnostic.source.path);
    xr_lir_lower_failure_free(&failure);
    source_fixture_free(&fixture);
    fail_at = SIZE_MAX;
}

static void storage_source_failures(void) {
    SourceFixture fixture = source_fixture("fn leaf(n:i64)->i64{return n+1}\n"
        "fn middle(n:i64)->i64{return leaf(n)}\nfn top(n:i64)->i64{return middle(n)}\n"
        "print(top(40))\nvar state=7\nprint(state)\n");
    attempts = 0; fail_at = SIZE_MAX;
    OK(run(&fixture));
    size_t total = attempts;
    CHECK(total > 10);
    for (size_t point = 0; point < total; ++point) {
        attempts = 0; fail_at = point;
        CHECK(run(&fixture) == XR_LIR_OUT_OF_MEMORY);
    }
    fail_at = SIZE_MAX;
    OK(run(&fixture));
    source_fixture_free(&fixture);
}

static void frame_cell_failures(void) {
    SourceFixture fixture=source_fixture("var text=\"start\"\nvar i=0\n"
        "while(i<3){text=\"next${i}\"\ni=i+1}\nprint(text)\n");
    attempts=0;fail_at=SIZE_MAX;OK(run(&fixture));
    size_t total=attempts;CHECK(total>10);
    for(size_t point=0;point<total;++point) {
        attempts=0;fail_at=point;
        CHECK(run(&fixture)==XR_LIR_OUT_OF_MEMORY);
    }
    fail_at=SIZE_MAX;OK(run(&fixture));source_fixture_free(&fixture);
    puts("physical frame Cell inventory failures reclaim every partial lowering workspace");
}

static void array_lower_failures(void) {
    SourceFixture fixture = source_fixture("var values=[1,2]\nfn change(n:i64){if(n>0){values.push(n)}}\n"
        "change(3)\nconst copy=values\nprint(copy[0])\n");
    attempts = 0; fail_at = SIZE_MAX; OK(run(&fixture));
    size_t total = attempts; CHECK(total > 10);
    for (size_t point = 0; point < total; ++point) {
        attempts = 0; fail_at = point;
        CHECK(run(&fixture) == XR_LIR_OUT_OF_MEMORY);
    }
    fail_at = SIZE_MAX; OK(run(&fixture));
    source_fixture_free(&fixture);
    printf("Array Lower workspace: %zu allocation failures preserve empty output and reclaim owners\n", total);
}

static void array_flow_failures(void) {
    SourceFixture fixture = source_fixture("fn fold(n:i64)->i64{var sum=0\nvar i=0\n"
        "while(i<n){var xs=[i]\nconst saved=xs\nif(i%2==0){xs.push(2)}else{xs[0]=3}\n"
        "if(i==1){i=i+1\ncontinue}\nsum=sum+saved[0]+xs[0]\ni=i+1}\nreturn sum}\nprint(fold(4))\n");
    attempts = 0; fail_at = SIZE_MAX; OK(run(&fixture));
    size_t total = attempts; CHECK(total > 50);
    for (size_t point = 0; point < total; ++point) {
        attempts = 0; fail_at = point;
        CHECK(run(&fixture) == XR_LIR_OUT_OF_MEMORY);
    }
    fail_at = SIZE_MAX; OK(run(&fixture)); source_fixture_free(&fixture);
    printf("Array live-out state: %zu allocation failures reclaim selected edges and shapes\n", total);
}

int main(void) {
    array_flow_failures();
    array_lower_failures();
    storage_source_failures();
    frame_cell_failures();
    attempts = 0;
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
    fixture = source_fixture("var n=1\nfn bump()->i64{n=n+1\nreturn n}\nprint(bump())\n");
    fail_at = SIZE_MAX;
    attempts = 0;
    OK(run(&fixture));
    CHECK(attempts > 2);
    for (size_t point=0; point<2; ++point) {
        attempts = 0; fail_at = point;
        CHECK(run(&fixture) == XR_LIR_OUT_OF_MEMORY);
        fail_at = SIZE_MAX;
        OK(run(&fixture));
    }
    source_fixture_free(&fixture);
    fixture = source_fixture("import time\nprint(1)\n");
    attempts = 0; fail_at = SIZE_MAX;
    OK(run(&fixture));
    size_t imported_attempts = attempts;
    CHECK(imported_attempts > 4);
    for (size_t point=0; point<imported_attempts; ++point) {
        attempts = 0; fail_at = point;
        CHECK(run(&fixture) == XR_LIR_OUT_OF_MEMORY);
        fail_at = SIZE_MAX;
        OK(run(&fixture));
    }
    source_fixture_free(&fixture);
    supported_length();
    constant_initializer_failures();
    diagnostic_copy_failure();
    CHECK(!lower_live && !lower_bytes);
    puts("representative lowering workspace and diagnostic-copy failures passed");
    return 0;
}
