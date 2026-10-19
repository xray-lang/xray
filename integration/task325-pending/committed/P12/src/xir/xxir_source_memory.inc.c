/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_memory.inc.c - One owner for zeroed and completely copied data
 */
static void *source_memory_allocate(SourceContext *ctx, size_t count, size_t size, bool zero) {
    if (ctx->diagnostic.status != XR_XIR_OK) return NULL;
    if (!size || count > (SIZE_MAX - sizeof(SourceMemory)) / size) {
        source_fail(ctx, NULL, XR_XIR_BUDGET, "source metadata budget exhausted"); return NULL;
    }
    size_t bytes = sizeof(SourceMemory) + count * size;
    XrXirStatus status = XR_XIR_OK;
    SourceMemory *memory = zero ? xir_compile_calloc(&ctx->compile, 1, bytes, &status) :
        xir_compile_alloc(&ctx->compile, bytes, &status);
    if (!memory) { source_fail(ctx, NULL, status, "source allocation failed"); return NULL; }
    /* A complete-copy receiver initializes its ownership links, then pays
     * for every payload byte in the caller's copy before publication. */
    if (!zero && !xir_compile_work(&ctx->compile,sizeof(*memory)+(ctx->memory?sizeof(void *):0))) {
        xr_compile_resources_free(memory);
        source_fail(ctx,NULL,XR_XIR_BUDGET,"source allocation links exhausted work");return NULL;
    }
    memory->next = ctx->memory; memory->previous = &ctx->memory;
    if (ctx->memory) ctx->memory->previous = &memory->next;
    ctx->memory = memory;
    return memory + 1;
}
static void *source_alloc(SourceContext *ctx, size_t count, size_t size) {
    return source_memory_allocate(ctx,count,size,true);
}
static void *source_reserve(SourceContext *ctx, size_t count, size_t size) {
    return source_memory_allocate(ctx,count,size,false);
}
