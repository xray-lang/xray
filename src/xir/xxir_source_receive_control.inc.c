/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_receive_control.inc.c - Request-local receiving workspaces
 *
 * KEY CONCEPT:
 *   Receiving arrays use the existing Source memory chain. Initialization
 *   finishes outside allocator locks before a block joins that chain.
 */
#ifndef XXIR_SOURCE_RECEIVE_CONTROL_INC_C
#define XXIR_SOURCE_RECEIVE_CONTROL_INC_C
#include "xxir_receive_control.h"
static bool source_receive_step(SourceContext *ctx,uint32_t *progress) {
    if (ctx->diagnostic.status!=XR_XIR_OK) return false;
    XrXirStatus status=xir_receive_step(ctx->request,progress);
    return status==XR_XIR_OK || source_fail(ctx,NULL,status,"Source receiving interrupted");
}
static void *source_receive_alloc(SourceContext *ctx,size_t count,size_t size) {
    if (ctx->diagnostic.status!=XR_XIR_OK) return NULL;
    if (!size || count>(SIZE_MAX-sizeof(SourceMemory))/size) {
        source_fail(ctx,NULL,XR_XIR_BUDGET,"Source receiving size exhausted");return NULL;
    }
    XrXirStatus status=XR_XIR_OK;
    SourceMemory *memory=xir_type_copy_zero(&ctx->compile,1,sizeof(*memory)+count*size,&status,ctx->request);
    if (!memory) {source_fail(ctx,NULL,status,"Source receiving allocation failed");return NULL;}
    memory->next=ctx->memory;memory->previous=&ctx->memory;
    if (ctx->memory) ctx->memory->previous=&memory->next;
    ctx->memory=memory;return memory+1;
}
#endif // XXIR_SOURCE_RECEIVE_CONTROL_INC_C
