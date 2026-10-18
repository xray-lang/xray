/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * retired.h - Complete preceding packets are rejected before allocation
 */
#ifndef XIR_COMPOSED_RETIRED_H
#define XIR_COMPOSED_RETIRED_H
#include "xir/xxir_checked.h"

typedef struct ComposedRetiredPacket { const void *bytes;size_t length; } ComposedRetiredPacket;

static inline void composed_retired_reject(const XrXirCompileContext *context,
    const ComposedRetiredPacket *packets,size_t count) {
    for (size_t i=0;i<count;++i) {
        XrCompileResourceStats before={0},after={0};
        CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
        XrXirArtifact *artifact=NULL;
        XrXirDiagnostic diagnostic={0};
        CHECK(xr_xir_compile_checked_read(context,packets[i].bytes,packets[i].length,&artifact,&diagnostic)==XR_XIR_BAD_STRUCTURE);
        CHECK(!artifact && diagnostic.status==XR_XIR_BAD_STRUCTURE);
        CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(after.allocation_count==before.allocation_count && after.allocated_bytes==before.allocated_bytes &&
            after.live_bytes==before.live_bytes);
        artifact=(XrXirArtifact *)(uintptr_t)1;
        CHECK(xr_xir_compile_checked_read(context,packets[i].bytes,packets[i].length,&artifact,NULL)==XR_XIR_BAD_STRUCTURE);
        CHECK(artifact==(XrXirArtifact *)(uintptr_t)1);
        CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(after.allocation_count==before.allocation_count && after.allocated_bytes==before.allocated_bytes &&
            after.live_bytes==before.live_bytes);
    }
}
#endif // XIR_COMPOSED_RETIRED_H
