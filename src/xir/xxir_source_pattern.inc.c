/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_pattern.inc.c - Typed constructor patterns and field authority
 *
 * KEY CONCEPT:
 *   Checking, coverage and projection share the same substituted pattern types.
 */
typedef struct SourceMatchPattern {
    AstNode *node, *binding;
    SourceEnumSelection selection;
    struct SourceMatchPattern **fields;
    XrXirType type;
    uint32_t count;
    bool any, boolean, truth;
} SourceMatchPattern;
static bool source_pattern_binding(AstNode *node) {
    return node && node->type == AST_PATTERN_LITERAL && node->as.pattern_literal.value &&
        node->as.pattern_literal.value->type == AST_VARIABLE;
}
static bool source_pattern_field_types(SourceContext *ctx, XrXirType type, uint32_t variant,
    XrXirType **output, uint32_t *count) {
    XrXirNominalType nominal=xr_xir_type_node(&ctx->types,type)->nominal;
    const XrXirNominalDeclaration *d=&ctx->nominals.declarations[nominal.declaration];
    XrXirNominalVariant v=d->variants[variant];
    *count=v.field_count; *output=NULL;
    if (!*count) return true;
    XrXirType *types=source_alloc(ctx,*count,sizeof(*types));
    if (!types) return false;
    SourceSubstitution sub={nominal.arguments,nominal.argument_count};
    for (uint32_t i=0;i<*count;++i)
        if (!source_work(ctx,NULL) || !source_substitute(ctx,&sub,d->fields[v.field_begin+i].type,0,&types[i])) return false;
    *output=types; return true;
}
static bool source_match_pattern(SourceContext *ctx, AstNode *node, XrXirType type,
    SourceMatchPattern *out, uint32_t depth) {
    if (!node || !source_work(ctx,node)) return false;
    if (depth>=128) return source_fail(ctx,node,XR_XIR_BUDGET,"pattern depth exhausted");
    out->node=node; out->type=type;
    if (node->type==AST_PATTERN_WILDCARD || source_pattern_binding(node)) {
        out->any=true;
        if (source_pattern_binding(node)) out->binding=node->as.pattern_literal.value;
        return true;
    }
    AstNode *path=NULL; PatternAdtNode *payload=NULL;
    if (node->type==AST_PATTERN_ADT) {payload=&node->as.pattern_adt;path=payload->variant;}
    else if (node->type==AST_PATTERN_LITERAL) path=node->as.pattern_literal.value;
    if (path && (path->type==AST_LITERAL_TRUE || path->type==AST_LITERAL_FALSE)) {
        if (type!=XR_XIR_BOOL) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"bool pattern requires bool");
        out->boolean=true; out->truth=path->type==AST_LITERAL_TRUE; return true;
    }
    if (!xr_xir_type_is_enum(&ctx->types,type) || !path || path->type!=AST_MEMBER_ACCESS)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"pattern family is not admitted");
    SourceTypeArguments arguments={0}; SourceName *owner=NULL,*binding=NULL;
    if (!source_nominal_path(ctx,path->as.member_access.object,&arguments,&binding,&owner)) return false;
    uint32_t declaration=xr_xir_type_node(&ctx->types,type)->nominal.declaration;
    if (!owner || owner->kind!=SOURCE_NOMINAL || owner->index!=declaration)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"pattern must name the scrutinee enum declaration");
    XrXirType actual=type;
    if (arguments.count && (!source_nominal_apply(ctx,owner,arguments.refs,arguments.count,&actual) || actual!=type))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"pattern type arguments differ from the scrutinee");
    const XrXirNominalDeclaration *d=&ctx->nominals.declarations[declaration];
    uint32_t v=0;
    for (;v<d->variant_count;++v) {
        if (!source_work(ctx,node)) return false;
        if (strlen(path->as.member_access.name)==d->variants[v].name.length &&
            !memcmp(path->as.member_access.name,d->variants[v].name.bytes,d->variants[v].name.length)) break;
    }
    if (v==d->variant_count || (payload!=NULL)!=(d->variants[v].field_count!=0))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"pattern variant or payload syntax mismatch");
    out->selection=(SourceEnumSelection){owner,binding,path->as.member_access.object,type,v};
    if (!source_query_reference(ctx,out->selection.path,binding,owner,XR_XIR_SOURCE_TYPE_USE) ||
        !source_query_target_reference(ctx,source_query_range(ctx,path,NULL),ctx->nominal_variants[declaration][v],XR_XIR_SOURCE_READ)) return false;
    if (!payload) return true;
    if (payload->count<0) return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"invalid pattern field count");
    XrXirType *types=NULL;
    if (!source_pattern_field_types(ctx,type,v,&types,&out->count)) return false;
    out->fields=source_alloc(ctx,out->count,sizeof(*out->fields));
    if (!out->fields) return false;
    XrXirNominalVariant variant=d->variants[v];
    for (int i=0;i<payload->count;++i) {
        const char *name=payload->field_names[i]; uint32_t field=0;
        for (;field<variant.field_count;++field) {
            if (!source_work(ctx,node)) return false;
            XrXirLiteral spelling=d->fields[variant.field_begin+field].name;
            if (strlen(name)==spelling.length && !memcmp(name,spelling.bytes,spelling.length)) break;
        }
        if (field==variant.field_count || out->fields[field])
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"unknown or duplicate pattern field");
        XrNameSpan span=payload->field_name_spans[i];
        XrXirSourceRange range={ctx->module,span.line,span.column,span.line,span.column};
        if (span.column>0 && strlen(name)<=(size_t)(INT_MAX-span.column)) range.end_column+=(int)strlen(name);
        if (!source_query_target_reference(ctx,range,ctx->nominal_members[declaration][variant.field_begin+field],XR_XIR_SOURCE_READ)) return false;
        SourceMatchPattern *child=source_alloc(ctx,1,sizeof(*child));
        if (!child || !source_match_pattern(ctx,payload->patterns[i],types[field],child,depth+1)) return false;
        out->fields[field]=child;
    }
    return true;
}
