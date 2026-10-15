/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_type_resources.c - Type retirement and read-only Source allocation faults
 *
 * KEY CONCEPT:
 *   Every real compiler allocation may fail without publishing partial Source.
 *   Cumulative allocation, peak live and work bounds stay independent.
 */
#include "xir/xxir_source.h"
#include "toolchain/xcompiler_session.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
typedef struct Allocation { void *pointer; size_t bytes; } Allocation;
static Allocation allocations[20000];
static size_t attempts, fail_at = SIZE_MAX;
static uint64_t physical;
static void *observed_alloc(size_t bytes) {
    if (attempts++ == fail_at) return NULL;
    void *memory = xr_malloc(bytes); if (!memory) return NULL;
    size_t i = 0; while (i < 20000 && allocations[i].pointer) ++i;
    CHECK(i < 20000); allocations[i] = (Allocation){memory, bytes}; physical += bytes;
    return memory;
}
static void observed_free(void *memory) {
    if (!memory) return;
    size_t i = 0; while (i < 20000 && allocations[i].pointer != memory) ++i;
    CHECK(i < 20000); physical -= allocations[i].bytes; allocations[i] = (Allocation){0};
    xr_free(memory);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc(n) observed_alloc(n)
#define xr_free(p) observed_free(p)
#include "base/xcompile_resources.c"

typedef struct SourceCase { const char *source; XrXirStatus status; } SourceCase;
static const SourceCase cases[] = {
    {"struct Box{const value:i64 init(value){this.value=value}}\n"
     "class Counter{value:i64 init(value){this.value=value} bump()->i64{this.value+=1;return this.value}}\n"
     "export fn referenceAnswer()->i64{const box=Box(11);const counter=Counter(20);var source=[5,6];const snapshot=source;source[0]=40;var current=snapshot;current[0]=9;return box.value+counter.bump()+snapshot[0]*100+len(snapshot)*10+current[0]}\n", XR_XIR_OK},
    {"export fn referenceAnswer(value:const i64)->i64{return value}", XR_XIR_BAD_STRUCTURE},
    {"type Choice=i64|const string\nexport fn referenceAnswer()->i64{return 7}", XR_XIR_BAD_STRUCTURE},
    {"struct Box{const value:i64 init(value){this.value=value}}\nexport fn referenceAnswer()->i64{var box=Box(3);box.value=8;return box.value}", XR_XIR_BAD_TYPE},
};
static const XrCompileResourceLimits limits = {
    UINT64_C(64)*1024*1024, UINT64_C(8)*1024*1024, UINT64_C(128000000)};

static XrXirStatus pipeline(const SourceCase *test, const XrCompileResourceLimits *bounds, XrCompileResourceStats *stats) {
    XrCompileResources *resources = NULL; XrCompilerSession *session = NULL;
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrCompileResourceStatus opened = xr_compile_resources_new(bounds, &resources);
    if (opened != XR_COMPILE_RESOURCE_OK) {
        CHECK(!resources && !physical);
        return opened == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    }
    XrXirStatus status = XR_XIR_OK;
    XrCompilerSessionStatus session_status = xr_compile_session_new(resources, &session);
    if (session_status != XR_COMPILER_SESSION_OK) {
        status = session_status == XR_COMPILER_SESSION_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
        goto done;
    }
    XrXirCompileContext context = {resources, xr_xir_compile_default_limits()};
    const XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_MEMORY, "type-fi", NULL};
    const XrXirSourceRequest request = {session, NULL, &authority, &context, NULL, NULL, XR_XIR_PROGRAM, NULL,NULL};
    const XrXirSourceText text = {NULL, test->source, strlen(test->source)};
    status = xr_xir_compile_source_check_text(&request, &text, &result, &diagnostic, NULL);
    CHECK(diagnostic.status == status);
    if (status == XR_XIR_OK) CHECK(result.checked && result.snapshot);
    else CHECK(!result.checked && !result.snapshot);
done:
    xr_xir_compile_source_result_free(&result); xr_compile_session_free(session);
    CHECK(xr_compile_resources_stats(resources, stats) == XR_COMPILE_RESOURCE_OK);
    xr_compile_resources_release(resources); CHECK(!physical);
    return status;
}

static void qualify(const SourceCase *test, unsigned ordinal) {
    XrCompileResourceStats normal = {0}, observed = {0};
    attempts = 0; fail_at = SIZE_MAX;
    CHECK(pipeline(test, &limits, &normal) == test->status);
    size_t sites = attempts; CHECK(sites && sites < 20000);
    printf("type source case=%u sites=%zu allocated=%llu peak=%llu work=%llu\n", ordinal, sites,
        (unsigned long long)normal.allocated_bytes, (unsigned long long)normal.peak_bytes,
        (unsigned long long)normal.work);
    for (size_t i = 0; i < sites; ++i) {
        attempts = 0; fail_at = i;
        XrXirStatus status = pipeline(test, &limits, &observed);
        if (status != XR_XIR_OUT_OF_MEMORY) fprintf(stderr, "case=%u site=%zu status=%u\n", ordinal, i, (unsigned)status);
        CHECK(status == XR_XIR_OUT_OF_MEMORY);
    }
    fail_at = SIZE_MAX;
    XrCompileResourceLimits exact = {normal.allocated_bytes, normal.peak_bytes, normal.work};
    attempts = 0; CHECK(pipeline(test, &exact, &observed) == test->status);
    CHECK(observed.allocated_bytes == normal.allocated_bytes && observed.peak_bytes == normal.peak_bytes && observed.work == normal.work);
    for (unsigned axis = 0; axis < 3; ++axis) {
        XrCompileResourceLimits short_limit = exact;
        if (!axis) --short_limit.allocated_bytes;
        else if (axis == 1) --short_limit.live_bytes;
        else --short_limit.work;
        attempts = 0; CHECK(pipeline(test, &short_limit, &observed) == XR_XIR_BUDGET);
    }
    CHECK(!physical);
}

int main(void) {
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) qualify(&cases[i], i);
    puts("Four real Source roles: full physical FI, exact/minus-one budgets and no partial publication passed");
    return 0;
}
