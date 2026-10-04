/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_compile_owner.c - Observe real allocation across compiler owner transitions
 */
#include "xir/xxir.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_types.h"
#include "xir/xxir_constraints.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
typedef struct Allocation { void *pointer; size_t bytes; } Allocation;
static Allocation allocations[4096];
static size_t attempts, fail_at = SIZE_MAX, live, total, peak, live_count;
static void *observe_alloc(size_t bytes) {
    if (attempts++ == fail_at) return NULL;
    void *memory = xr_malloc(bytes);
    CHECK(memory);
    size_t i = 0;
    while (i < 4096 && allocations[i].pointer) ++i;
    CHECK(i < 4096);
    allocations[i] = (Allocation){memory, bytes};
    ++live_count; live += bytes; total += bytes;
    if (live > peak) peak = live;
    return memory;
}
static void observe_free(void *memory) {
    if (!memory) return;
    size_t i = 0;
    while (i < 4096 && allocations[i].pointer != memory) ++i;
    CHECK(i < 4096);
    live -= allocations[i].bytes; --live_count;
    allocations[i] = (Allocation){0};
    xr_free(memory);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) observe_alloc(bytes)
#define xr_free(memory) observe_free(memory)
#include "base/xcompile_resources.c"

static XrCompileResourceStats stats(XrCompileResources *owner) {
    XrCompileResourceStats s;
    CHECK(xr_compile_resources_stats(owner, &s) == XR_COMPILE_RESOURCE_OK);
    CHECK(s.live_bytes == live && s.allocated_bytes == total && s.peak_bytes == peak);
    return s;
}
static void reset(size_t failure) {
    CHECK(!live && !live_count);
    attempts = total = peak = 0; fail_at = failure;
}
static const XrCompileResourceLimits unlimited = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
static const XrXirInstruction instructions[] = {
    {XR_XIR_CONST_INT, XR_XIR_I64, {0,0}, {0,0}, 42, {0,0}},
    {XR_XIR_RETURN, XR_XIR_UNIT, {0,0}, {0,0}, 0, {0,0}}
};
static const XrXirBlock simple_blocks[] = {{0,2,0,0}};
static const XrXirFunction function = {"main",4,NULL,0,XR_XIR_I64,simple_blocks,1,instructions,2,NULL,0};
static const XrXirModule module = {XR_XIR_BUILT,&function,1,NULL,NULL,NULL,NULL,XR_XIR_PROGRAM,NULL};
static const XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};

#include "fixtures.h"

static XrXirStatus pipeline(XrCompileResources *owner, bool release_caller, unsigned kind) {
    XrXirCompileContext context = {owner,xr_xir_compile_default_limits()};
    XrXirArtifact *checked = NULL, *specialized = NULL, *decoded = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0};
    XrXirEffects *effects = NULL;
    XrXirStatus status = kind == 4 ? generic_error_built(&context,&checked) : kind == 3 ? implementation_built(&context,&checked) : kind == 2 ? nominal_built(&context,&checked) : kind == 1 ? generic_built(&context,&checked) : xr_xir_compile_check(&context,&module,&checked,NULL);
    if (status == XR_XIR_OK && kind == 4) {
        status = xr_xir_compile_effects_analyze(checked,&effects);
        if (status == XR_XIR_OK) CHECK(xr_xir_effects_error(effects,1,(XrXirType)257,1));
        xr_xir_compile_effects_free(effects); effects = NULL;
    }
    if (status == XR_XIR_OK) status = xr_xir_compile_specialize(checked,&specialized,NULL);
    if (status == XR_XIR_OK) status = xr_xir_compile_checked_write(specialized,&packet,NULL);
    if (status == XR_XIR_OK) status = xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&decoded,NULL);
    if (status == XR_XIR_OK) status = xr_xir_compile_lower(decoded,&target,&lowered,NULL);
    if (status == XR_XIR_OK) status = xr_xir_compile_artifact_verify(lowered,NULL);
    if (status == XR_XIR_OK) status = xr_xir_compile_effects_analyze(decoded,&effects);
    if (status == XR_XIR_OK) {
        CHECK(xr_xir_compile_artifact_context(checked)->resources == owner);
        CHECK(xr_xir_compile_artifact_context(specialized)->resources == owner);
        CHECK(xr_xir_compile_artifact_context(decoded)->resources == owner);
        CHECK(xr_xir_compile_artifact_context(lowered)->resources == owner);
        if (!kind) CHECK(xr_xir_compile_artifact_module(lowered)->functions[0].instructions[0].immediate == 42);
    }

    (void)stats(owner);
    xr_xir_compile_artifact_free(checked);
    xr_xir_compile_artifact_free(specialized);
    xr_xir_compile_artifact_free(decoded);
    xr_xir_compile_artifact_free(lowered);
    xr_xir_compile_effects_free(effects);
    if (release_caller && status == XR_XIR_OK) {
        xr_compile_resources_release(owner);
        CHECK(packet.length >= 64 && !memcmp(packet.bytes,"XRCHK",5));
        CHECK(live_count == 2);
    }
    xr_xir_compile_checked_packet_free(&packet);
    return status;
}
static void resource_boundaries(unsigned kind, XrCompileResourceStats measured) {
    for (unsigned metric = 0; metric < 3; ++metric) {
        uint64_t exact = metric == 0 ? measured.allocated_bytes : metric == 1 ? measured.peak_bytes : measured.work;
        for (unsigned below = 0; below < 2; ++below) {
            reset(SIZE_MAX);
            XrCompileResourceLimits limits = unlimited;
            if (metric == 0) limits.allocated_bytes = exact - below;
            if (metric == 1) limits.live_bytes = exact - below;
            if (metric == 2) limits.work = exact - below;
            XrCompileResources *owner = NULL;
            CHECK(xr_compile_resources_new(&limits,&owner) == XR_COMPILE_RESOURCE_OK);
            XrXirStatus result = pipeline(owner,false,kind);
            if (result != (below ? XR_XIR_BUDGET : XR_XIR_OK)) fprintf(stderr,"metric=%u below=%u kind=%u status=%d exact=%llu\n",metric,below,kind,result,(unsigned long long)exact);
            CHECK(result == (below ? XR_XIR_BUDGET : XR_XIR_OK));
            CHECK(live_count == 1);
            xr_compile_resources_release(owner);
        }
    }
    /* Distributed work failures reach each algorithm family and preserve physical cleanup. */
    uint64_t step = measured.work / 128 + 1;
    for (uint64_t limit = 1; limit < measured.work; limit += step) {
        reset(SIZE_MAX);
        XrCompileResourceLimits limits = unlimited; limits.work = limit;
        XrCompileResources *owner = NULL;
        CHECK(xr_compile_resources_new(&limits,&owner) == XR_COMPILE_RESOURCE_OK);
        CHECK(pipeline(owner,false,kind) == XR_XIR_BUDGET);
        CHECK(live_count == 1);
        xr_compile_resources_release(owner);
    }
}
static void pipeline_failures(unsigned kind) {
    reset(SIZE_MAX);
    XrCompileResources *owner = NULL;
    CHECK(xr_compile_resources_new(&unlimited,&owner) == XR_COMPILE_RESOURCE_OK);
    XrXirStatus status = pipeline(owner,false,kind);
    if (status != XR_XIR_OK) fprintf(stderr,"pipeline status=%d\n",status);
    CHECK(status == XR_XIR_OK);
    size_t count = attempts;
    XrCompileResourceStats measured = stats(owner);
    CHECK(live_count == 1 && measured.allocation_count == attempts);
    xr_compile_resources_release(owner);
    resource_boundaries(kind,measured);
    for (size_t failure = 1; failure < count; ++failure) {
        reset(failure); owner = NULL;
        CHECK(xr_compile_resources_new(&unlimited,&owner) == XR_COMPILE_RESOURCE_OK);
        status = pipeline(owner,false,kind);
        if (status != XR_XIR_OUT_OF_MEMORY) fprintf(stderr,"failure=%zu status=%d attempts=%zu\n",failure,status,attempts);
        CHECK(status == XR_XIR_OUT_OF_MEMORY);
        CHECK(live_count == 1);
        xr_compile_resources_release(owner);
    }
    reset(SIZE_MAX); owner = NULL;
    CHECK(xr_compile_resources_new(&unlimited,&owner) == XR_COMPILE_RESOURCE_OK);
    CHECK(pipeline(owner,true,kind) == XR_XIR_OK);
    CHECK(!live && !live_count);
    printf("pipeline %u allocations=%zu allocated=%llu peak=%llu work=%llu; every allocation OOM; physical zero\n",
        kind,count,(unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,
        (unsigned long long)measured.work);
}
#include "resource_cases.h"
#include "reader_lower_cases.h"

int main(void) {
    mandatory_context();
    reader_lower_cases();
    constraint_formula();
    exhausted_owner();
    structural_caps();
    inference_failures();
    for (unsigned kind = 0; kind < 3; ++kind) metadata_failures(kind);
    for (unsigned kind = 0; kind < 5; ++kind) pipeline_failures(kind);
    return 0;
}
