/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Source copies provenance into its receiver before the checked artifact owns it. */
static bool source_map_literal_append(SourceContext *ctx,AstNode *site,const XrXirMapLiteral *input,uint32_t *ordinal) {
    XrXirMapLiteral literal=*input;
    if((literal.file.length!=0)!=(literal.file.bytes!=NULL) || literal.count>65536 ||
        (literal.count && !literal.items) || literal.module>=ctx->module_count)
        return source_fail(ctx,site,XR_XIR_BAD_STRUCTURE,"Map literal provenance is incomplete");
    if(ctx->map_literal_count>=ctx->compile.limits.instructions)
        return source_fail(ctx,site,XR_XIR_BUDGET,"Map literal inventory exhausted");
    char *path=literal.file.length?source_alloc(ctx,literal.file.length,1):NULL;
    XrXirMapLiteralOrigin *items=literal.count?source_alloc(ctx,literal.count,sizeof(*items)):NULL;
    if((literal.file.length && !path) || (literal.count && !items) ||
        !source_copy_bytes(ctx,site,path,literal.file.bytes,literal.file.length) ||
        !source_copy_bytes(ctx,site,items,literal.items,(size_t)literal.count*sizeof(*items)))return false;
    literal.file.bytes=path;literal.items=items;
    uint32_t index=ctx->map_literal_count;
    XrXirMapLiteral *table=source_query_append(ctx,ctx->map_literals,&ctx->map_literal_count,
        &ctx->map_literal_capacity,sizeof(*table));
    if(!table)return false;
    ctx->map_literals=table;table[index]=literal;*ordinal=index;return true;
}
static bool source_map_literal_metadata(SourceContext *ctx,AstNode *node,uint32_t *ordinal) {
    MapLiteralNode *map=&node->as.map_literal;uint32_t count=(uint32_t)map->count;
    XrXirMapLiteralOrigin *items=count?source_alloc(ctx,count,sizeof(*items)):NULL;
    if(count && !items)return false;
    for(uint32_t n=0;n<count;++n) {
        AstNode *key=map->keys[n]?map->keys[n]:map->values[n];
        XrNameSpan span=map->entry_spans?map->entry_spans[n].key:(XrNameSpan){key->line,key->column};
        if(span.line<=0 || span.column<=0)return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Map literal entry has no original source position");
        items[n]=(XrXirMapLiteralOrigin){map->keys[n]?0u:1u,(uint32_t)span.line,(uint32_t)span.column};
    }
    const char *path=ctx->graph->specs[ctx->module].source_path;
    size_t length=path?source_text_size(ctx,path):0;
    if(length>UINT32_MAX)return source_fail(ctx,node,XR_XIR_BUDGET,"Map source path exceeds format limit");
    XrXirMapLiteral literal={{path,(uint32_t)length},items,count,ctx->module};
    return source_map_literal_append(ctx,node,&literal,ordinal);
}
