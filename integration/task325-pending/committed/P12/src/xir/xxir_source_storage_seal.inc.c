/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_storage_seal.inc.c - Compact finalized private Source inventories
 *
 * KEY CONCEPT:
 *   Stable record identities retain only their used vectors after construction.
 */
static const void *source_storage_compact(SourceContext *ctx, const void *array,
    uint32_t count, uint32_t *capacity, size_t size) {
    if (count==*capacity) return array;
    if (count>*capacity || !count || !array) {
        source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source query capacity is inconsistent");return NULL;
    }
    void *compact=source_reserve(ctx,count,size);
    if (!compact || !source_copy_bytes(ctx,NULL,compact,array,(size_t)count*size)) return NULL;
    source_vector_retire(ctx,array);*capacity=count;return compact;
}
static bool source_query_seal_storage(SourceContext *ctx) {
    ctx->query.declarations=source_storage_compact(ctx,ctx->query.declarations,
        ctx->query.declaration_count,&ctx->declaration_capacity,sizeof(*ctx->query.declarations));
    ctx->query.references=source_storage_compact(ctx,ctx->query.references,
        ctx->query.reference_count,&ctx->reference_capacity,sizeof(*ctx->query.references));
    ctx->query.expressions=source_storage_compact(ctx,ctx->query.expressions,
        ctx->query.expression_count,&ctx->expression_capacity,sizeof(*ctx->query.expressions));
    ctx->syntax.declarations=source_storage_compact(ctx,ctx->syntax.declarations,
        ctx->syntax.declaration_count,&ctx->syntax_declaration_capacity,sizeof(*ctx->syntax.declarations));
    ctx->syntax.references=source_storage_compact(ctx,ctx->syntax.references,
        ctx->syntax.reference_count,&ctx->syntax_reference_capacity,sizeof(*ctx->syntax.references));
    return ctx->diagnostic.status==XR_XIR_OK;
}
static bool source_function_seal_storage(SourceContext *ctx) {
    uint32_t original=ctx->function_capacity,capacity=original;
    ctx->functions=(XrXirFunction *)source_storage_compact(ctx,ctx->functions,
        ctx->function_count,&capacity,sizeof(*ctx->functions));
    capacity=original;
    ctx->bodies=(SourceFunction *)source_storage_compact(ctx,ctx->bodies,
        ctx->function_count,&capacity,sizeof(*ctx->bodies));
    capacity=original;
    ctx->identities=(XrXirFunctionIdentity *)source_storage_compact(ctx,ctx->identities,
        ctx->function_count,&capacity,sizeof(*ctx->identities));
    capacity=original;
    ctx->generics=(XrXirGeneric *)source_storage_compact(ctx,ctx->generics,
        ctx->function_count,&capacity,sizeof(*ctx->generics));
    if (ctx->diagnostic.status!=XR_XIR_OK) return false;
    ctx->function_capacity=ctx->function_count;return true;
}
