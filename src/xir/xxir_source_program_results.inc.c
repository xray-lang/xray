/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_program_results.inc.c - Real closed calls supply owned literal data
 *
 * KEY CONCEPT:
 *   Exact failed selections retain their source site without executing again.
 *   Zero actual instances need an inventory, never a fabricated execution.
 */
/* Query-only display types may be appended afterward. Their earlier IDs are
 * the unchanged original prefix, while the final construction owns new keys. */
static bool source_program_query_types(SourceContext *ctx,const XrXirArtifact *checked) {
    const XrXirTypes *types=checked->module.types;
    if (!types) return true;
    if (types->count<ctx->types.count) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"comptime receiving lost original query types");
    XrXirTypeNode *nodes=types->count?source_alloc(ctx,types->count,sizeof(*nodes)):NULL;
    if ((types->count && !nodes) || !source_copy_bytes(ctx,NULL,nodes,types->nodes,(size_t)types->count*sizeof(*nodes))) return false;
    ctx->types=*types;ctx->types.nodes=nodes;ctx->type_capacity=types->count;
    if (types->nominals) { ctx->nominals=*types->nominals;ctx->types.nominals=&ctx->nominals; }
    if (types->interfaces) { ctx->interfaces=*types->interfaces;ctx->types.interfaces=&ctx->interfaces; }
    return true;
}
