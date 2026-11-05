/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_read_workspace_probe.h - Real ordinary resource and physical owner observations
 */
#ifndef XIR_READ_WORKSPACE_PROBE_H
#define XIR_READ_WORKSPACE_PROBE_H
#include "xir_construction_fixture.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_declarations.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#c);exit(1);}} while(0)

typedef struct ReadPhysical {void *pointer;size_t bytes;} ReadPhysical;
static ReadPhysical pc_physical[2048];
static size_t live,live_bytes,attempts,fail_at=SIZE_MAX;
static bool injected;
static void (*pc_free_hook)(void *,size_t);
static void (*pc_allocate_hook)(void *,size_t);
static void *pc_malloc(size_t bytes) {
    if(attempts++==fail_at){injected=true;return NULL;}
    void *pointer=xr_malloc(bytes);if(!pointer)return NULL;
    CHECK(live<2048 && bytes<=SIZE_MAX-live_bytes);
    pc_physical[live++]=(ReadPhysical){pointer,bytes};live_bytes+=bytes;
    if(pc_allocate_hook)pc_allocate_hook(pointer,bytes);return pointer;
}
static void pc_free(void *pointer) {
    if(!pointer)return;
    size_t i=0;while(i<live && pc_physical[i].pointer!=pointer)++i;
    CHECK(i<live && live_bytes>=pc_physical[i].bytes);
    if(pc_free_hook)pc_free_hook(pointer,pc_physical[i].bytes);
    live_bytes-=pc_physical[i].bytes;pc_physical[i]=pc_physical[--live];xr_free(pointer);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) pc_malloc(bytes)
#define xr_free(pointer) pc_free(pointer)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")
#include "xir/xxir_effects.c"

static XrCompileResourceLimits pc_caps(void) {return (XrCompileResourceLimits){XR_COMPILE_MEMORY_ORDINARY,0,false};}
static XrCompileResourceStats pc_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);return stats;
}
static XrXirCompileContext pc_owner(XrCompileResourceLimits limits) {
    CHECK(!live && !live_bytes);XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();return context;
}
static void pc_release(XrXirCompileContext *context,uint64_t held) {
    CHECK(pc_stats(context).held_bytes==held);xr_compile_resources_release(context->resources);
    *context=(XrXirCompileContext){0};CHECK(!live && !live_bytes);
}
#endif // XIR_READ_WORKSPACE_PROBE_H
