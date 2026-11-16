/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_nominal_reserve.inc.c - Owned nominal declaration vector growth
 */
static bool source_nominal_reserve(SourceContext *ctx) {
    if(ctx->nominals.count<ctx->nominal_capacity)return true;
    uint32_t capacity=ctx->nominal_capacity?ctx->nominal_capacity*2:8;
    if(capacity<ctx->nominal_capacity)return source_fail(ctx,NULL,XR_XIR_OVERFLOW,"nominal declaration capacity overflow");
    XrXirNominalDeclaration *declarations=source_alloc(ctx,capacity,sizeof(*declarations));
    SourceName **sources=source_alloc(ctx,capacity,sizeof(*sources)),**methods=source_alloc(ctx,capacity,sizeof(*methods));
    uint32_t **members=source_alloc(ctx,capacity,sizeof(*members)),**variants=source_alloc(ctx,capacity,sizeof(*variants));
    if(!declarations||!sources||!methods||!members||!variants)return false;
    size_t n=ctx->nominals.count;
    if(!source_copy_bytes(ctx,NULL,declarations,ctx->nominals.declarations,n*sizeof(*declarations))||
        !source_copy_bytes(ctx,NULL,sources,ctx->nominal_sources,n*sizeof(*sources))||
        !source_copy_bytes(ctx,NULL,methods,ctx->nominal_methods,n*sizeof(*methods))||
        !source_copy_bytes(ctx,NULL,members,ctx->nominal_members,n*sizeof(*members))||
        !source_copy_bytes(ctx,NULL,variants,ctx->nominal_variants,n*sizeof(*variants)))return false;
    source_retire_array(ctx,ctx->nominals.declarations);source_retire_array(ctx,ctx->nominal_sources);
    source_retire_array(ctx,ctx->nominal_methods);source_retire_array(ctx,ctx->nominal_members);source_retire_array(ctx,ctx->nominal_variants);
    ctx->nominals.declarations=declarations;ctx->nominal_sources=sources;ctx->nominal_methods=methods;
    ctx->nominal_members=members;ctx->nominal_variants=variants;ctx->nominal_capacity=capacity;ctx->types.nominals=&ctx->nominals;
    return true;
}
