/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_slot_declarations.inc.c - Owned exported constant identities
 *
 * KEY CONCEPT:
 *   Source and received constants use the same visible immutable slot facts.
 */
static bool source_slot_declare(SourceContext *ctx,SourceName *symbol,AstNode *node,
    bool mutable,XrXirSourceRange range,uint32_t slot) {
    if (node->is_exported && mutable)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"exported mutable state is not admitted");
    if (slot>=ctx->slot_count)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"module slot inventory is inconsistent");
    uint32_t name=0;size_t length=0;
    if (node->is_exported && (!source_text_length(ctx,node,symbol->name,&length) ||
        !source_literal_append(ctx,node,symbol->name,length,&name))) return false;
    symbol->kind=SOURCE_SLOT;symbol->index=slot;symbol->module=ctx->module;symbol->mutable=mutable;
    if (!source_query_declare(ctx,symbol,XR_XIR_SOURCE_BINDING,0,range)) return false;
    XrXirSourceDeclaration *query=(XrXirSourceDeclaration *)ctx->query.declarations;
    query[symbol->declaration-1].exported=node->is_exported;
    ctx->slots[slot]=(XrXirSlot){ctx->module,XR_XIR_UNIT,mutable,node->is_exported,name};
    return true;
}
