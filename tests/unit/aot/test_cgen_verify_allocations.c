/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_cgen_verify_allocations.c - Actual verifier failure and release witnesses
 *
 * KEY CONCEPT:
 *   Instrument the real verifier and exercise both XIR emission boundaries.
 */
#include "base/xmalloc.h"
#include "../xir/xir_execution_fixture.h"
#include "../xir/xir_call_fixture.h"
#include "xir/xxir_emit_c.h"
#include "lir/xi_cgen_verify_output.h"
#include <stdint.h>
#ifdef XR_OS_WINDOWS
#include <windows.h>
#endif

typedef struct Allocation { void *pointer; size_t bytes; } Allocation;
/* Artifact graphs coexist with verifier scratch; this fixed observation table
 * does not allocate or consume the fault-injection sequence. */
static Allocation allocations[32768];
static size_t calls, physical_calls, live, bytes_live, fail_at = SIZE_MAX;
static bool verifier_allocation;
static void *counted_malloc(size_t size) {
    ++physical_calls;
    if (verifier_allocation && calls++ == fail_at) return NULL;
    void *pointer = xr_malloc(size);
    if (pointer) {
        CHECK(live < sizeof(allocations) / sizeof(allocations[0]));
        allocations[live++] = (Allocation){pointer, size}; bytes_live += size;
    }
    return pointer;
}
static void counted_free(void *pointer) {
    if (pointer) {
        size_t index = 0;
        while (index < live && allocations[index].pointer != pointer) ++index;
        CHECK(index < live); bytes_live -= allocations[index].bytes;
        allocations[index] = allocations[--live];
    }
    xr_free(pointer);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc counted_malloc
#define xr_free counted_free
/* Include the current production allocation entry point, not an old verifier
 * allocator. Linked production emitters resolve these same resource symbols. */
#include "../../../src/base/xcompile_resources.c"
#undef xr_malloc
#undef xr_free

/* Keep verifier fault indices separate from fixture construction/emission.
 * Each wrapper calls the real production ledger; failures occur in its actual
 * malloc, including metadata headers and resize allocate-before-free. */
static XrCompileResourceStatus verifier_alloc(XrCompileResources *resources,
    size_t bytes, void **output) {
    CHECK(!verifier_allocation); verifier_allocation = true;
    XrCompileResourceStatus status = xr_compile_resources_alloc(resources, bytes, output);
    verifier_allocation = false; return status;
}
static XrCompileResourceStatus verifier_calloc(XrCompileResources *resources,
    size_t count, size_t size, void **output) {
    CHECK(!verifier_allocation); verifier_allocation = true;
    XrCompileResourceStatus status = xr_compile_resources_calloc(resources, count, size, output);
    verifier_allocation = false; return status;
}
static XrCompileResourceStatus verifier_resize(XrCompileResources *resources,
    void **memory, size_t bytes) {
    CHECK(!verifier_allocation); verifier_allocation = true;
    XrCompileResourceStatus status = xr_compile_resources_resize(resources, memory, bytes);
    verifier_allocation = false; return status;
}
#define xr_compile_resources_alloc verifier_alloc
#define xr_compile_resources_calloc verifier_calloc
#define xr_compile_resources_resize verifier_resize
#include "../../../src/lir/xi_cgen_verify_output.c"
#undef xr_compile_resources_alloc
#undef xr_compile_resources_calloc
#undef xr_compile_resources_resize

static void physical_baseline(XrCompileResources *resources, size_t allocations_before,
    size_t bytes_before, uint64_t ledger_live_before) {
    XrCompileResourceStats stats;
    CHECK(xr_compile_resources_stats(resources, &stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(live == allocations_before && bytes_live == bytes_before);
    CHECK(stats.held_bytes == ledger_live_before && stats.held_bytes == bytes_live);
    CHECK(!verifier_allocation);
}
static void allocation_scan(XrCompileResources *resources, const char *source) {
    const size_t allocations_before = live, bytes_before = bytes_live;
    XrCompileResourceStats baseline;
    CHECK(xr_compile_resources_stats(resources, &baseline) == XR_COMPILE_RESOURCE_OK);
    calls=0;fail_at=SIZE_MAX;XiCgenVerifyResult result;
    CHECK(xr_compile_cgen_verify_output(resources, source,strlen(source),&result)==XI_CGEN_VERIFY_PASSED);
    CHECK(result.category==XI_CGEN_VERIFY_OK);
    physical_baseline(resources, allocations_before, bytes_before, baseline.held_bytes);
    size_t total=calls;CHECK(total>=3);
    for (size_t point=0;point<total;++point) {
        calls=0;fail_at=point;memset(&result,0xa5,sizeof(result));XiCgenVerifyResult saved=result;
        CHECK(xr_compile_cgen_verify_output(resources, source,strlen(source),&result)==XI_CGEN_VERIFY_OUT_OF_MEMORY);
        CHECK(!memcmp(&result,&saved,sizeof(result)) && calls == point + 1);
        physical_baseline(resources, allocations_before, bytes_before, baseline.held_bytes);
        calls=0;
        CHECK(xr_compile_cgen_verify_output_or_ice(resources, source,strlen(source),"allocation-test")==XI_CGEN_VERIFY_OUT_OF_MEMORY);
        CHECK(calls == point + 1);
        physical_baseline(resources, allocations_before, bytes_before, baseline.held_bytes);
    }
    printf("actual verifier allocation points=%zu scratch-physical=0\n",total);fail_at=SIZE_MAX;
}
static void emission_scan(const XrXirArtifact *artifact,bool resumable) {
    const XrXirCompileContext *context = xr_xir_compile_artifact_context(artifact);
    CHECK(context && context->resources);
    XrCompileResources *resources = context->resources;
    const size_t allocations_before = live, bytes_before = bytes_live;
    XrCompileResourceStats baseline;
    CHECK(xr_compile_resources_stats(resources, &baseline) == XR_COMPILE_RESOURCE_OK);
    fail_at=SIZE_MAX;calls=0;XrXirCSource output={0};
    XrXirStatus status=resumable ? xr_xir_compile_emit_c(artifact,"resource_test",1048576,&output) :
        xr_xir_compile_emit_leaf_c(artifact,"resource_test",1048576,&output);
    CHECK(status==XR_XIR_OK && output.text);
    size_t total=calls;CHECK(total>=1);xr_xir_compile_c_source_free(&output);
    physical_baseline(resources, allocations_before, bytes_before, baseline.held_bytes);
    for (size_t point=0;point<total;++point) {
        fail_at=point;calls=0;output=(XrXirCSource){NULL,0};XrXirCSource saved=output;
        status=resumable ? xr_xir_compile_emit_c(artifact,"resource_test",1048576,&output) :
            xr_xir_compile_emit_leaf_c(artifact,"resource_test",1048576,&output);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && !memcmp(&output,&saved,sizeof(output)) && calls == point + 1);
        physical_baseline(resources, allocations_before, bytes_before, baseline.held_bytes);
    }
    printf("actual %s emitter verifier points=%zu scratch-physical=0\n",resumable ? "resumable" : "leaf",total);fail_at=SIZE_MAX;
}
static void indexed_lines_fixture(char *source) {
    const char tail[] = "void f(void) {\n const char *s = \"{ )\";\n int v0 = 1;\n (void) v0;\n}\n";
    memset(source, '\n', 260);
    memcpy(source + 260, tail, sizeof(tail));
}
static XrCompileResourceStats indexed_lines_census(void) {
    CHECK(!live && !bytes_live);
    XrCompileResources *resources = NULL;
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, true};
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline, normal;
    CHECK(xr_compile_resources_stats(resources, &baseline) == XR_COMPILE_RESOURCE_OK);
    char source[512]; indexed_lines_fixture(source);
    XiCgenVerifyResult result = {0}; calls = 0;
    CHECK(xr_compile_cgen_verify_output(resources, source, strlen(source), &result) == XI_CGEN_VERIFY_PASSED);
    CHECK(result.category == XI_CGEN_VERIFY_OK);
    CHECK(xr_compile_resources_stats(resources, &normal) == XR_COMPILE_RESOURCE_OK);
    physical_baseline(resources, 1, baseline.held_bytes, baseline.held_bytes);
    printf("owned line spans census: sites=%zu allocated=%llu peak=%llu held=%llu baseline=%llu\n",
        calls, (unsigned long long)normal.allocated_bytes, (unsigned long long)normal.peak_bytes,
        (unsigned long long)normal.held_bytes,
        (unsigned long long)baseline.held_bytes);
    xr_compile_resources_release(resources);
    CHECK(!live && !bytes_live);
    return normal;
}
static void indexed_held_case(uint64_t peak, bool minus_one) {
    CHECK(peak > 0 && !live && !bytes_live && !verifier_allocation);
    fail_at = SIZE_MAX;
    XrCompileResources *resources = NULL;
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_FINITE, peak-(minus_one ? 1 : 0), true};
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline, previous;
    CHECK(xr_compile_resources_stats(resources, &baseline) == XR_COMPILE_RESOURCE_OK);
    const size_t blocks_before = live, bytes_before = bytes_live;
    physical_baseline(resources, blocks_before, bytes_before, baseline.held_bytes);
    previous = baseline;
    char source[512]; indexed_lines_fixture(source);
    for (size_t attempt = 0; attempt < 3; ++attempt) {
        XiCgenVerifyResult result, saved, zero = {0};
        memset(&result, 0xa5, sizeof(result)); saved = result; calls = 0;
        XiCgenVerifyStatus status = xr_compile_cgen_verify_output(resources, source, strlen(source), &result);
        if (minus_one) {
            CHECK(status == XI_CGEN_VERIFY_MEMORY_LIMIT);
            CHECK(!memcmp(&result, &saved, sizeof(result)));
        } else {
            CHECK(status == XI_CGEN_VERIFY_PASSED && calls > 0);
            CHECK(!memcmp(&result, &zero, sizeof(result)));
        }
        XrCompileResourceStats current;
        CHECK(xr_compile_resources_stats(resources, &current) == XR_COMPILE_RESOURCE_OK);
        CHECK(current.allocation_count >= previous.allocation_count);
        CHECK(current.allocated_bytes >= previous.allocated_bytes);
        CHECK(current.peak_bytes >= previous.peak_bytes && current.peak_bytes <= limits.memory_limit);
        physical_baseline(resources, blocks_before, bytes_before, baseline.held_bytes);
        previous = current;
    }
    xr_compile_resources_release(resources);
    CHECK(!live && !bytes_live && !verifier_allocation);
    printf("owned line held peak=%llu minus1=%u retries=3 final physical=0/0\n",
        (unsigned long long)peak, (unsigned)minus_one);
}
int main(int argc,char **argv) {
    scalar_compile_begin();
    XrCompileResources *resources = scalar_owner.context.resources;
    if (argc==2 && !strcmp(argv[1],"--ice")) {
#ifdef XR_OS_WINDOWS
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
        const char malformed[]="void f(void) {\n";
        (void)xr_compile_cgen_verify_output_or_ice(resources, malformed,sizeof(malformed)-1,"malformed-test");
        return 1;
    }
    CHECK(argc==1);
    const char body[]="void f(void) {\n int v0=1;\n int v2048=2;\n int v4096=3;\n return;\n}\n";
    const char macros[]="#define v0 0\n#define v2048 2\n#define v4096 3\nvoid f(void) {\n}\n";
    allocation_scan(resources,body);allocation_scan(resources,macros);
    char indexed[512]; indexed_lines_fixture(indexed); allocation_scan(resources,indexed);
    XiCgenVerifyResult result;memset(&result,0xcc,sizeof(result));XiCgenVerifyResult saved=result;
    size_t physical_before = physical_calls;
    calls=0;CHECK(xr_compile_cgen_verify_output(resources, NULL,1,&result)==XI_CGEN_VERIFY_BAD_ARGUMENT);
    CHECK(!calls && physical_calls == physical_before && !memcmp(&result,&saved,sizeof(result)));
    CHECK(xr_compile_cgen_verify_output_or_ice(resources, NULL,1,"invalid")==XI_CGEN_VERIFY_BAD_ARGUMENT);
    CHECK(xr_compile_cgen_verify_output(resources, NULL,0,&result)==XI_CGEN_VERIFY_PASSED);
    XiCgenVerifyResult zero={0};CHECK(!memcmp(&result,&zero,sizeof(result)));
    CHECK(xr_compile_cgen_verify_output(resources, body,sizeof(body)-1,NULL)==XI_CGEN_VERIFY_PASSED);
    physical_baseline(resources, 1, scalar_owner.baseline.held_bytes, scalar_owner.baseline.held_bytes);
    XrXirArtifact *artifact=fixture_lowered(&scalar_owner.context);emission_scan(artifact,false);xr_xir_compile_artifact_free(artifact);
    artifact=uninitialized_leaf_fixture(&scalar_owner.context);emission_scan(artifact,false);xr_xir_compile_artifact_free(artifact);
    artifact=call_fixture(&scalar_owner.context,0);emission_scan(artifact,true);xr_xir_compile_artifact_free(artifact);
    scalar_compile_owner_free(&scalar_owner);
    XrCompileResourceStats indexed_required = indexed_lines_census();
    indexed_held_case(indexed_required.peak_bytes, false);
    indexed_held_case(indexed_required.peak_bytes, true);
    CHECK(!live && !bytes_live && !verifier_allocation);
    puts("typed verifier OOM, unchanged outputs and physical release PASS");return 0;
}
