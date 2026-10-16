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
 * owned display facts cross the immutable snapshot boundary. */
static XrXirSourceRange source_query_name_span(SourceContext *ctx,XrNameSpan span,const char *name) {
    size_t length=source_text_size(ctx,name);
    if(span.line<=0||span.column<=0||!length||length>(size_t)(INT_MAX-span.column)) {
        source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source syntax name span is missing");
        return (XrXirSourceRange){ctx->module,0,0,0,0};
    }
    return (XrXirSourceRange){ctx->module,span.line,span.column,span.line,span.column+(int)length};
}
static XrXirSourceRange source_query_token_range(SourceContext *ctx,AstNode *node) {
    while(node&&(node->type==AST_CALL_EXPR||node->type==AST_ENUM_CONSTRUCT)) {
        if(!source_work(ctx,node))return (XrXirSourceRange){ctx->module,0,0,0,0};
        node=node->type==AST_CALL_EXPR?node->as.call_expr.callee:node->as.enum_construct.variant_path;
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
    XrXirSourceDeclarationSyntax fact={.name=range,.role=!range.line?XR_XIR_SOURCE_SYNTAX_NONE:
        range.line==range.end_line&&range.column>0&&range.end_column>range.column?
        XR_XIR_SOURCE_SYNTAX_NAME:XR_XIR_SOURCE_SYNTAX_UNRESOLVED};
    return source_copy_bytes(ctx,NULL,&facts[ctx->syntax.declaration_count-1],&fact,sizeof(fact));
}
static bool source_query_syntax_role(SourceContext *ctx,uint32_t declaration,XrXirSourceSyntaxRole role,uint32_t flags) {
    if(!declaration||declaration>ctx->syntax.declaration_count)return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"syntax declaration identity is invalid");
    XrXirSourceDeclarationSyntax *fact=(XrXirSourceDeclarationSyntax *)&ctx->syntax.declarations[declaration-1];
    XrXirSourceDeclarationSyntax changed=*fact;changed.role=role;changed.flags=flags;
    if(role==XR_XIR_SOURCE_SYNTAX_NONE||role==XR_XIR_SOURCE_SYNTAX_CLOSURE||role==XR_XIR_SOURCE_SYNTAX_RECEIVER)
        changed.name=(XrXirSourceRange){fact->name.module,0,0,0,0};
    return source_copy_bytes(ctx,NULL,fact,&changed,sizeof(changed));
}
static bool source_query_callable(SourceContext *ctx,uint32_t declaration) {
    if(!declaration||declaration>ctx->query.declaration_count)return false;
    const XrXirSourceDeclaration *decl=&ctx->query.declarations[declaration-1];
    return decl->kind==XR_XIR_SOURCE_FUNCTION||(decl->kind==XR_XIR_SOURCE_MEMBER&&
        ctx->syntax.declarations[declaration-1].role==XR_XIR_SOURCE_SYNTAX_METHOD);
}
static bool source_query_call(SourceContext *ctx,uint32_t reference,XrXirSourceRange range) {
    if(ctx->function>=ctx->function_count)return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source call body is invalid");
    uint32_t declaration=ctx->bodies[ctx->function].declaration;
    uint32_t body=source_query_callable(ctx,declaration)?declaration:0;
    uint32_t caller=body&&ctx->syntax.declarations[body-1].role!=XR_XIR_SOURCE_SYNTAX_NONE?body:0;
    bool known=range.line>0&&range.column>0&&range.end_line==range.line&&range.end_column>range.column;
    XrXirSourceCall call={.reference=reference,.caller=caller,.body=body,
        .range=known?range:(XrXirSourceRange){range.module,0,0,0,0},
        .module_initializer=ctx->function<ctx->module_count&&ctx->function==ctx->module,.selection_known=known};
    XrXirSourceCall *facts=source_query_append(ctx,ctx->calls.items,&ctx->calls.count,
        &ctx->call_capacity,sizeof(*facts));
    if(!facts)return false;ctx->calls.items=facts;
    return source_copy_bytes(ctx,NULL,&facts[ctx->calls.count-1],&call,sizeof(call));
}
static bool source_query_syntax_reference(SourceContext *ctx,XrXirSourceRange range) {
    XrXirSourceRange *facts=source_query_append(ctx,ctx->syntax.references,
        &ctx->syntax.reference_count,&ctx->syntax_reference_capacity,sizeof(*facts));
    if(!facts)return false;ctx->syntax.references=facts;
    if(!source_copy_bytes(ctx,NULL,&facts[ctx->syntax.reference_count-1],&range,sizeof(range)))return false;
    if(ctx->syntax.reference_count!=ctx->query.reference_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source reference observations diverged");
    uint32_t reference=ctx->query.reference_count-1;
    return ctx->query.references[reference].access!=XR_XIR_SOURCE_CALL||source_query_call(ctx,reference,range);
}
static bool source_query_pattern_binding(SourceContext *ctx,uint32_t declaration,XrXirSourceRange range) {
    XrXirSourceMarker *facts=source_query_append(ctx,ctx->syntax.markers,&ctx->syntax.marker_count,
        &ctx->syntax_marker_capacity,sizeof(*facts));
    if(!facts)return false;ctx->syntax.markers=facts;
    XrXirSourceMarker marker={range,declaration,XR_XIR_SOURCE_MARKER_PATTERN_FIELD_BINDING};
    return source_copy_bytes(ctx,NULL,&facts[ctx->syntax.marker_count-1],&marker,sizeof(marker));
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
