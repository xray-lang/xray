/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_syntax_collect.inc.c - Producer-owned syntax selections
 *
 * KEY CONCEPT:
 *   Collect observational roles and token ranges before the parser owner dies.
 */
/* Parser-owned selections are consumed while source checking is active. Only
 * scalar role/range facts cross the immutable snapshot boundary. */
static XrXirSourceRange source_query_name_span(SourceContext *ctx,XrNameSpan span,const char *name) {
    size_t length=source_text_size(ctx,name);
    if(span.line<=0||span.column<=0||!length||length>(size_t)(INT_MAX-span.column)) {
        source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source syntax name span is missing");
        return (XrXirSourceRange){ctx->module,0,0,0,0};
    }
    return (XrXirSourceRange){ctx->module,span.line,span.column,span.line,span.column+(int)length};
}
static XrXirSourceRange source_query_token_range(SourceContext *ctx,AstNode *node) {
    while(node&&node->type==AST_CALL_EXPR) {
        if(!source_work(ctx,node))return (XrXirSourceRange){ctx->module,0,0,0,0};
        node=node->as.call_expr.callee;
    }
    if(node&&node->type==AST_MEMBER_ACCESS&&node->as.member_access.name_span.line)
        return source_query_name_span(ctx,node->as.member_access.name_span,node->as.member_access.name);
    if(node&&node->type==AST_MEMBER_SET&&node->as.member_set.name_span.line)
        return source_query_name_span(ctx,node->as.member_set.name_span,node->as.member_set.member);
    if(node&&node->type==AST_COMPOUND_ASSIGNMENT&&node->as.compound_assignment.name_span.line)
        return source_query_name_span(ctx,node->as.compound_assignment.name_span,node->as.compound_assignment.name);
    if(node&&node->line>0&&node->column>0&&
        (node->type==AST_ASSIGNMENT||node->type==AST_VARIABLE||node->type==AST_THIS_EXPR)) {
        const char *name=node->type==AST_ASSIGNMENT?node->as.assignment.name:
            node->type==AST_VARIABLE?node->as.variable.name:"this";
        return source_query_name_span(ctx,(XrNameSpan){node->line,node->column},name);
    }
    /* Other reference collection sites already carry one spelling node or an
     * explicit type/payload span. Query consumers still validate lexical bounds. */
    return source_query_range(ctx,node,NULL);
}
static bool source_query_syntax_declaration(SourceContext *ctx,XrXirSourceRange range) {
    XrXirSourceDeclarationSyntax *facts=source_query_append(ctx,ctx->syntax.declarations,
        &ctx->syntax.declaration_count,&ctx->syntax_declaration_capacity,sizeof(*facts));
    if(!facts)return false;ctx->syntax.declarations=facts;
    XrXirSourceDeclarationSyntax fact={range,!range.line?XR_XIR_SOURCE_SYNTAX_NONE:
        range.line==range.end_line&&range.column>0&&range.end_column>range.column?
        XR_XIR_SOURCE_SYNTAX_NAME:XR_XIR_SOURCE_SYNTAX_UNRESOLVED,0};
    return source_copy_bytes(ctx,NULL,&facts[ctx->syntax.declaration_count-1],&fact,sizeof(fact));
}
static bool source_query_syntax_role(SourceContext *ctx,uint32_t declaration,XrXirSourceSyntaxRole role,uint32_t flags) {
    if(!declaration||declaration>ctx->syntax.declaration_count)return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"syntax declaration identity is invalid");
    XrXirSourceDeclarationSyntax *fact=(XrXirSourceDeclarationSyntax *)&ctx->syntax.declarations[declaration-1];
    XrXirSourceDeclarationSyntax changed=*fact;changed.role=role;changed.flags=flags;
    if(role==XR_XIR_SOURCE_SYNTAX_CLOSURE||role==XR_XIR_SOURCE_SYNTAX_RECEIVER)
        changed.name=(XrXirSourceRange){fact->name.module,0,0,0,0};
    return source_copy_bytes(ctx,NULL,fact,&changed,sizeof(changed));
}
static bool source_query_syntax_reference(SourceContext *ctx,XrXirSourceRange range) {
    XrXirSourceRange *facts=source_query_append(ctx,ctx->syntax.references,
        &ctx->syntax.reference_count,&ctx->syntax_reference_capacity,sizeof(*facts));
    if(!facts)return false;ctx->syntax.references=facts;
    return source_copy_bytes(ctx,NULL,&facts[ctx->syntax.reference_count-1],&range,sizeof(range));
}
static bool source_query_mode(SourceContext *ctx,uint32_t declaration,XrParamMode mode,XrNameSpan span) {
    if(mode==XR_PARAM_READ)return true;
    if(mode!=XR_PARAM_REF&&mode!=XR_PARAM_MOVE)return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"unknown source parameter mode");
    XrXirSourceRange range=source_query_name_span(ctx,span,mode==XR_PARAM_REF?"ref":"move");
    if(ctx->diagnostic.status!=XR_XIR_OK)return false;
    XrXirSourceMarker *facts=source_query_append(ctx,ctx->syntax.markers,&ctx->syntax.marker_count,
        &ctx->syntax_marker_capacity,sizeof(*facts));
    if(!facts)return false;ctx->syntax.markers=facts;
    XrXirSourceMarker marker={range,declaration,
        mode==XR_PARAM_REF?XR_XIR_SOURCE_MARKER_REF:XR_XIR_SOURCE_MARKER_MOVE};
    return source_copy_bytes(ctx,NULL,&facts[ctx->syntax.marker_count-1],&marker,sizeof(marker));
}
static bool source_query_ref_argument(SourceContext *ctx,uint32_t declaration,XrNameSpan span) {
    XrXirSourceRange range=source_query_name_span(ctx,span,"ref");
    if(ctx->diagnostic.status!=XR_XIR_OK)return false;
    XrXirSourceMarker *facts=source_query_append(ctx,ctx->syntax.markers,&ctx->syntax.marker_count,
        &ctx->syntax_marker_capacity,sizeof(*facts));
    if(!facts)return false;ctx->syntax.markers=facts;
    XrXirSourceMarker marker={range,declaration,XR_XIR_SOURCE_MARKER_REF_ARGUMENT};
    return source_copy_bytes(ctx,NULL,&facts[ctx->syntax.marker_count-1],&marker,sizeof(marker));
}
