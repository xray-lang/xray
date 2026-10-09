/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cgen_growth_cases.h - Bounded growth with independent byte and fee oracles
 */
#ifndef XIR_CGEN_GROWTH_CASES_H
#define XIR_CGEN_GROWTH_CASES_H

enum { GROWTH_BYTES = 1848, GROWTH_LIMIT = 1849 };

static char growth_byte(size_t position) {
    return (char)(unsigned char)(position % 256);
}

static void growth_fill(CBuffer *buffer) {
    for (size_t i = 0; i < GROWTH_BYTES && buffer->status == XR_XIR_OK; ++i) {
        char *before = buffer->text;
        size_t capacity = buffer->capacity, length = buffer->length;
        emit_payload_byte(buffer,growth_byte(i));
        if (buffer->status != XR_XIR_OK) {
            CHECK(buffer->text == before && buffer->capacity == capacity);
            CHECK(buffer->length == length);
        }
    }
    for (size_t i = 0; i < buffer->length; ++i) CHECK(buffer->text[i] == growth_byte(i));
}

static void growth_free(CBuffer *buffer, XrCompileResources *resources) {
    xr_compile_resources_free(buffer->text);
    XrCompileResourceStats stats;
    CHECK(xr_compile_resources_stats(resources,&stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats.live_bytes == sizeof(XrCompileResources));
    xr_compile_resources_release(resources);
    CHECK(!runtime_live && !runtime_bytes);
}

static void growth_actual_faults(void) {
    static const size_t prefix[] = {0,127,255,511,1023};
    static const size_t capacity[] = {0,128,256,512,1024};
    size_t sites = 0;
    for (size_t fault = SIZE_MAX;;) {
        XrCompileResourceLimits limits = {UINT64_C(1)<<20,UINT64_C(1)<<20,UINT64_C(1)<<25};
        XrCompileResources *resources = NULL;
        CHECK(xr_compile_resources_new(&limits,&resources) == XR_COMPILE_RESOURCE_OK);
        XrXirCompileContext context = {resources,xr_xir_compile_default_limits()};
        CBuffer buffer = {.context=&context,.limit=GROWTH_LIMIT,.status=XR_XIR_OK};
        runtime_attempts = 0;runtime_fail_at = fault;
        growth_fill(&buffer);
        runtime_fail_at = SIZE_MAX;
        if (fault == SIZE_MAX) {
            CHECK(buffer.status == XR_XIR_OK && buffer.length == GROWTH_BYTES);
            CHECK(buffer.capacity == GROWTH_LIMIT);
            sites = runtime_attempts;
            CHECK(sites == sizeof(prefix)/sizeof(prefix[0]));
            CHECK(emit_finalize(&buffer) && buffer.text[GROWTH_BYTES] == 0);
        } else {
            CHECK(buffer.status == XR_XIR_OUT_OF_MEMORY && runtime_attempts == fault+1);
            CHECK(buffer.length == prefix[fault] && buffer.capacity == capacity[fault]);
            XrCompileResourceStats before,after;
            CHECK(xr_compile_resources_stats(resources,&before) == XR_COMPILE_RESOURCE_OK);
            char *owned = buffer.text;
            emit_payload_byte(&buffer,'!');
            CHECK(!emit_finalize(&buffer));
            CHECK(xr_compile_resources_stats(resources,&after) == XR_COMPILE_RESOURCE_OK);
            CHECK(buffer.status == XR_XIR_OUT_OF_MEMORY && buffer.text == owned);
            CHECK(buffer.length == prefix[fault] && buffer.capacity == capacity[fault]);
            CHECK(after.work == before.work && after.allocated_bytes == before.allocated_bytes);
            CHECK(runtime_attempts == fault+1);
        }
        growth_free(&buffer,resources);
        if (fault == SIZE_MAX) fault=0;else if (++fault == sites) break;
    }
    printf("C growth: %zu actual resize OOM sites, retained prefixes and physical zero PASS\n",sites);
}

static void growth_resource_axes(void) {
    /* The five payloads are 128,256,512,1024,1849. Their sum is 3769;
     * copies total 1920. Peak overlap is 1024+1849 plus two headers.
     * Work is ledger1 + stores1848 + copies1920 + alloc5 + double3 + NUL1. */
    const uint64_t allocated = sizeof(XrCompileResources)+3769+5*sizeof(CompileAllocation);
    const uint64_t live = sizeof(XrCompileResources)+2873+2*sizeof(CompileAllocation);
    const uint64_t work = 3778;
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        XrCompileResourceLimits limits = {UINT64_C(1)<<20,UINT64_C(1)<<20,UINT64_C(1)<<25};
        if (axis == 0) limits.allocated_bytes = allocated-minus;
        if (axis == 1) limits.live_bytes = live-minus;
        if (axis == 2) limits.work = work-minus;
        XrCompileResources *resources = NULL;
        CHECK(xr_compile_resources_new(&limits,&resources) == XR_COMPILE_RESOURCE_OK);
        XrXirCompileContext context = {resources,xr_xir_compile_default_limits()};
        CBuffer buffer = {.context=&context,.limit=GROWTH_LIMIT,.status=XR_XIR_OK};
        runtime_attempts = 0;
        growth_fill(&buffer);
        if (!minus || axis == 2) {
            CHECK(buffer.status == XR_XIR_OK && buffer.length == GROWTH_BYTES);
            CHECK(buffer.capacity == GROWTH_LIMIT && runtime_attempts == 5);
            buffer.text[buffer.length] = 'q';
            CHECK(emit_finalize(&buffer) == !minus);
            CHECK(buffer.text[buffer.length] == (minus ? 'q' : 0));
        } else {
            CHECK(buffer.status == XR_XIR_BUDGET && buffer.length == 1023);
            CHECK(buffer.capacity == 1024 && runtime_attempts == 4);
        }
        CHECK(buffer.status == (minus ? XR_XIR_BUDGET : XR_XIR_OK));
        XrCompileResourceStats stats;
        CHECK(xr_compile_resources_stats(resources,&stats) == XR_COMPILE_RESOURCE_OK);
        if (!minus || axis == 2) {
            CHECK(stats.allocated_bytes == allocated && stats.peak_bytes == live);
            CHECK(stats.work == work-minus && stats.allocation_count == 6);
        } else {
            CHECK(stats.allocated_bytes == sizeof(XrCompileResources)+1920+4*sizeof(CompileAllocation));
            CHECK(stats.peak_bytes == sizeof(XrCompileResources)+1536+2*sizeof(CompileAllocation));
            CHECK(stats.work == 1927 && stats.allocation_count == 5);
        }
        growth_free(&buffer,resources);
    }
}

static void growth_boundary_windows(void) {
    for (size_t limit = 0; limit < 2; ++limit) {
        XrCompileResourceLimits limits = {UINT64_C(1)<<20,UINT64_C(1)<<20,UINT64_C(1)<<25};
        XrCompileResources *resources = NULL;
        CHECK(xr_compile_resources_new(&limits,&resources) == XR_COMPILE_RESOURCE_OK);
        XrXirCompileContext context = {resources,xr_xir_compile_default_limits()};
        CBuffer buffer = {.context=&context,.limit=limit,.status=XR_XIR_OK};
        runtime_attempts = 0;emit_payload_byte(&buffer,'a');
        CHECK(buffer.status == XR_XIR_BUDGET && !buffer.text && !buffer.capacity && !buffer.length);
        CHECK(!runtime_attempts);
        XrCompileResourceStats stats;
        CHECK(xr_compile_resources_stats(resources,&stats) == XR_COMPILE_RESOURCE_OK);
        CHECK(stats.work == 1 && stats.allocation_count == 1);
        growth_free(&buffer,resources);
    }
    XrCompileResourceLimits limits = {UINT64_C(1)<<20,UINT64_C(1)<<20,UINT64_C(1)<<25};
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits,&resources) == XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context = {resources,xr_xir_compile_default_limits()};
    void *memory = NULL;
    CHECK(xr_compile_resources_alloc(resources,GROWTH_LIMIT,&memory) == XR_COMPILE_RESOURCE_OK);
    memset(memory,'q',GROWTH_LIMIT);
    CBuffer buffer = {.text=memory,.capacity=GROWTH_LIMIT,.context=&context,
        .limit=GROWTH_LIMIT,.status=XR_XIR_OK};
    runtime_attempts = 0;growth_fill(&buffer);
    CHECK(buffer.status == XR_XIR_OK && buffer.text == memory && !runtime_attempts);
    CHECK(buffer.length == GROWTH_BYTES && buffer.text[GROWTH_BYTES] == 'q');
    CHECK(emit_finalize(&buffer) && buffer.text[GROWTH_BYTES] == 0);
    XrCompileResourceStats stats;
    CHECK(xr_compile_resources_stats(resources,&stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats.work == 1851 && stats.allocation_count == 2);
    CHECK(stats.allocated_bytes == sizeof(XrCompileResources)+GROWTH_LIMIT+sizeof(CompileAllocation));
    growth_free(&buffer,resources);
}

static void cgen_growth_cases(void) {
    growth_actual_faults();
    growth_resource_axes();
    growth_boundary_windows();
    puts("C growth: independent all-byte oracle, three resource axes and 0/1/window controls PASS");
}
#endif // XIR_CGEN_GROWTH_CASES_H
