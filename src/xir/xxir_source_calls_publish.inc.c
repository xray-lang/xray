/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_calls_publish.inc.c - Copy actual source body bounds at publication
 */
static XrXirStatus source_calls_publish(SourceContext *ctx,XrXirSourceSnapshot *snapshot) {
    uint32_t count=ctx->query.declaration_count;
    XrXirSourceFunctionRange *functions=count?source_alloc(ctx,count,sizeof(*functions)):NULL;
    if(count&&!functions)return ctx->diagnostic.status;
    for(uint32_t i=0;i<ctx->function_count;++i) {
        if(!source_work(ctx,NULL))return ctx->diagnostic.status;
        const SourceFunction *body=&ctx->bodies[i];
        const AstNode *node=body->node;
        if(!source_query_callable(ctx,body->declaration)||!node)continue;
        if(node->type!=AST_FUNCTION_DECL&&node->type!=AST_FUNCTION_EXPR&&node->type!=AST_METHOD_DECL)continue;
        XrXirSourceRange range={body->module,node->line,node->column,node->end_line,node->end_column};
        /* Missing parser bounds remain unknown; no synthetic whole-file range. */
        if(range.line<=0||range.column<=0||range.end_line<range.line||range.end_column<=0||
            (range.end_line==range.line&&range.end_column<=range.column))continue;
        XrXirSourceFunctionRange *fact=&functions[body->declaration-1];
        if(fact->known&&(fact->range.module!=range.module||fact->range.line!=range.line||
            fact->range.column!=range.column||fact->range.end_line!=range.end_line||fact->range.end_column!=range.end_column))
            return XR_XIR_BAD_STRUCTURE;
        XrXirSourceFunctionRange value={range,true};
        if(!source_copy_bytes(ctx,NULL,fact,&value,sizeof(value)))return ctx->diagnostic.status;
    }
    XrXirSourceCalls calls=ctx->calls;calls.functions=functions;calls.declaration_count=count;
    return xr_xir_compile_source_snapshot_calls_copy(snapshot,&calls);
}
