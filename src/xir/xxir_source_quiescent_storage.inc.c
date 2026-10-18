/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_quiescent_storage.inc.c - Compact a completed private inventory
 *
 * KEY CONCEPT:
 *   Construction-local element borrows have ended before a Built view or
 *   refinement workspace exists. Child payloads retain their original owners;
 *   later query and root updates resolve the unchanged numeric identities.
 */
static void *source_compact_array(SourceContext *ctx,const void *array,uint32_t count,
    uint32_t *capacity,size_t size) {
    if (!ctx || ctx->diagnostic.status!=XR_XIR_OK) return NULL;
    if (!capacity || !size || count>*capacity || (!array && *capacity)) {
        source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source storage inventory is inconsistent");return NULL;
    }
    if (!source_work_units(ctx,NULL,3+3*sizeof(void *)+2*sizeof(uint32_t))) return NULL;
    if (!count) {
        if (array) source_release_private((void *)array);
        *capacity=0;return NULL;
    }
    if (count==*capacity) return (void *)array;
    void *tight=source_alloc(ctx,count,size);
    if (!tight || !source_copy_bytes(ctx,NULL,tight,array,(size_t)count*size)) return NULL;
    source_release_private((void *)array);*capacity=count;return tight;
}

/* This sole production call follows every complete declaration/body builder
 * and precedes source_module_view. No active frame retains table elements.
 * SourceRootQuery, SourceName and comptime sites retain ordinals, while their
 * strings, parameters and other child allocations stay on the same ledger. */
static bool source_quiescent_storage(SourceContext *ctx) {
    if (!ctx || ctx->diagnostic.status!=XR_XIR_OK) return false;
    if (ctx->root_flow || ctx->function_count>ctx->function_capacity ||
        ctx->next_closure!=ctx->closure_limit)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source storage still has active construction readers");
    if (!source_work_units(ctx,NULL,5*sizeof(uint32_t)+sizeof(void *)+4)) return false;
    uint32_t capacity=ctx->function_capacity;
#define SOURCE_COMPACT(field,count,limit) do { \
    void *tight=source_compact_array(ctx,ctx->field,(count),&(limit),sizeof(*ctx->field)); \
    if (ctx->diagnostic.status!=XR_XIR_OK) return false; \
    ctx->field=tight; \
} while (0)
    SOURCE_COMPACT(functions,ctx->function_count,capacity);
    capacity=ctx->function_capacity;
    SOURCE_COMPACT(bodies,ctx->function_count,capacity);
    capacity=ctx->function_capacity;
    SOURCE_COMPACT(identities,ctx->function_count,capacity);
    capacity=ctx->function_capacity;
    SOURCE_COMPACT(generics,ctx->function_count,capacity);
    ctx->function_capacity=ctx->function_count;
    SOURCE_COMPACT(query.modules,ctx->query.module_count,ctx->query_module_capacity);
    SOURCE_COMPACT(query.declarations,ctx->query.declaration_count,ctx->declaration_capacity);
    SOURCE_COMPACT(query.references,ctx->query.reference_count,ctx->reference_capacity);
    SOURCE_COMPACT(query.expressions,ctx->query.expression_count,ctx->expression_capacity);
    SOURCE_COMPACT(syntax.declarations,ctx->syntax.declaration_count,ctx->syntax_declaration_capacity);
    SOURCE_COMPACT(syntax.references,ctx->syntax.reference_count,ctx->syntax_reference_capacity);
    SOURCE_COMPACT(syntax.markers,ctx->syntax.marker_count,ctx->syntax_marker_capacity);
#undef SOURCE_COMPACT
    return true;
}
