/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_consumer_context_owner.h - Finite graph and independent probe ownership
 *
 * KEY CONCEPT:
 *   Each registered operation owns a real ledger; artifacts retain their graph.
 */
#ifndef XIR_CONSUMER_CONTEXT_OWNER_H
#define XIR_CONSUMER_CONTEXT_OWNER_H
#include "xir/xxir_compile_context.h"
#ifndef XIR_CONSUMER_COUNTED_RESOURCES
#include "xir_source_program_compile_owner.h"
#endif
typedef struct ConsumerContextOwner { XrXirCompileContext suite_context;XrCompileResourceStats baseline; } ConsumerContextOwner;
static ConsumerContextOwner consumer_context_owners[1024];
static size_t consumer_context_owner_count;
static XrXirCompileContext consumer_context;
static const XrXirCompileContext *suite_context=&consumer_context;
static XrCompileResourceStats consumer_context_stats(const XrXirCompileContext *ctx) {
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(ctx->resources,&stats)==XR_COMPILE_RESOURCE_OK);return stats;
}
static inline XrXirCompileContext consumer_context_ephemeral(XrCompileResourceLimits limits) {
    XrXirCompileContext ctx={0};
    CHECK(limits.allocated_bytes<=67108864 && limits.live_bytes<=8388608 && limits.work<=128000000);
    CHECK(xr_compile_resources_new(&limits,&ctx.resources)==XR_COMPILE_RESOURCE_OK);
    ctx.limits=xr_xir_compile_default_limits();return ctx;
}
static inline void consumer_context_ephemeral_free(XrXirCompileContext *ctx,XrCompileResourceStats baseline) {
    CHECK(consumer_context_stats(ctx).live_bytes==baseline.live_bytes);
    xr_compile_resources_release(ctx->resources);*ctx=(XrXirCompileContext){0};
}
static XrXirCompileContext consumer_context_limits(XrCompileResourceLimits limits) {
    CHECK(consumer_context_owner_count<1024);
    CHECK(limits.allocated_bytes<=UINT64_C(67108864) && limits.live_bytes<=UINT64_C(8388608) && limits.work<=UINT64_C(128000000));
    ConsumerContextOwner *owner=&consumer_context_owners[consumer_context_owner_count++];
    CHECK(xr_compile_resources_new(&limits,&owner->suite_context.resources)==XR_COMPILE_RESOURCE_OK);
    owner->suite_context.limits=xr_xir_compile_default_limits();owner->baseline=consumer_context_stats(&owner->suite_context);
    return owner->suite_context;
}
static XrXirCompileContext consumer_context_default(void) {
    return consumer_context_limits((XrCompileResourceLimits){UINT64_C(67108864),UINT64_C(8388608),UINT64_C(128000000)});
}
static uint64_t consumer_context_ledger_bytes(void) {
    uint64_t bytes=0;for(size_t i=0;i<consumer_context_owner_count;++i)bytes+=consumer_context_owners[i].baseline.live_bytes;return bytes;
}
static void consumer_contexts_free(void) {
#ifdef XIR_CONSUMER_COUNTED_RESOURCES
    CHECK(live==consumer_context_owner_count && live_bytes==consumer_context_ledger_bytes());
#else
    CHECK(source_program_compile_live==consumer_context_owner_count && source_program_compile_bytes==consumer_context_ledger_bytes());
#endif
    uint64_t max_allocated=0,max_work=0,max_peak=0;
    for(size_t i=0;i<consumer_context_owner_count;++i) {
        ConsumerContextOwner *owner=&consumer_context_owners[i];XrCompileResourceStats stats=consumer_context_stats(&owner->suite_context);
        CHECK(stats.live_bytes==owner->baseline.live_bytes);
        if(stats.allocated_bytes>max_allocated)max_allocated=stats.allocated_bytes;
        if(stats.work>max_work)max_work=stats.work;
        if(stats.peak_bytes>max_peak)max_peak=stats.peak_bytes;
        xr_compile_resources_release(owner->suite_context.resources);*owner=(ConsumerContextOwner){0};
    }
#ifdef XIR_CONSUMER_COUNTED_RESOURCES
    CHECK(!live && !live_bytes);
#else
    CHECK(!source_program_compile_live && !source_program_compile_bytes);
    xr_free(source_program_compile_allocations);source_program_compile_allocations=NULL;
    source_program_compile_capacity=0;
#endif
    fprintf(stderr,"consumer compiler owners=%zu maxima allocated/work/peak=%llu/%llu/%llu; physical=0/0\n",
        consumer_context_owner_count,(unsigned long long)max_allocated,(unsigned long long)max_work,(unsigned long long)max_peak);
    consumer_context_owner_count=0;consumer_context=(XrXirCompileContext){0};
}
#endif // XIR_CONSUMER_CONTEXT_OWNER_H
