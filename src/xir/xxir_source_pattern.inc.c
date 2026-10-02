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
    AstNode *node, *binding, *literal;
    SourceEnumSelection selection;
    SourceName *symbol;
    struct SourceMatchPattern **fields;
    XrXirType type;
    uint32_t count;
    bool any, boolean, truth;
    bool interval, empty;
    uint64_t low, high;
    int64_t literal_payload;
} SourceMatchPattern;
static uint64_t source_pattern_integer_max(XrXirType type) {
    uint32_t width=xr_xir_integer_bits(type);
    return width==64?UINT64_MAX:(UINT64_C(1)<<width)-1;
}
static uint64_t source_pattern_integer_bias(XrXirType type) {
    return xr_xir_integer_signed(type)?UINT64_C(1)<<(xr_xir_integer_bits(type)-1):0;
}
static bool source_pattern_integer_key(SourceContext *ctx, AstNode *node, XrXirType type, uint64_t *key) {
    SourceInteger literal; uint64_t bits;
    if (!source_direct_integer(ctx,node,&literal)) return false;
    if (!literal.present) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"range endpoint constant is not admitted");
    if (!source_integer_payload(ctx,node,&literal,type,&bits)) return false;
    *key=(bits&source_pattern_integer_max(type))^source_pattern_integer_bias(type);
    return true;
}
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
    if (node->type==AST_PATTERN_RANGE) {
        PatternRangeNode *range=&node->as.pattern_range;
        if (!xr_xir_type_is_integer(type)) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"range pattern requires integer fields");
        if (!source_pattern_integer_key(ctx,range->start,type,&out->low) ||
            !source_pattern_integer_key(ctx,range->end,type,&out->high) ||
            !source_query_expression(ctx,range->start,type) || !source_query_expression(ctx,range->end,type)) return false;
        out->interval=true;
        out->empty=range->inclusive_end?out->low>out->high:out->low>=out->high;
        if (!range->inclusive_end && !out->empty) --out->high;
        return true;
    }
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
        out->boolean=true; out->truth=path->type==AST_LITERAL_TRUE;
        return source_query_expression(ctx,path,type);
    }
    AstNode *atom=path;
    if (atom && atom->type==AST_UNARY_NEG) atom=atom->as.unary.operand;
    if (atom && (atom->type==AST_LITERAL_INT || atom->type==AST_LITERAL_FLOAT ||
        (atom==path && atom->type==AST_LITERAL_STRING))) {
        bool admitted=atom->type==AST_LITERAL_INT?(xr_xir_type_is_integer(type) || xr_xir_float_bits(type)):
            atom->type==AST_LITERAL_FLOAT?xr_xir_float_bits(type)!=0:type==XR_XIR_STRING;
        if (!admitted) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"literal pattern type differs from its field");
        out->literal=path;
        uint64_t bits=0;
        if (atom->type==AST_LITERAL_INT) {
            SourceInteger integer;
            if (!source_direct_integer(ctx,path,&integer) || !integer.present) return false;
            if (xr_xir_type_is_integer(type)) {
                if (!source_integer_payload(ctx,path,&integer,type,&bits)) return false;
                out->interval=true;
                out->low=(bits&source_pattern_integer_max(type))^source_pattern_integer_bias(type); out->high=out->low;
            } else if (!source_integer_float_payload(ctx,path,&integer,type,&bits)) return false;
        } else if (atom->type==AST_LITERAL_FLOAT) {
            SourceDecimal decimal;
            if (!source_direct_decimal(ctx,path,&decimal) || !decimal.node ||
                !source_decimal_payload(ctx,&decimal,type,&bits)) return false;
        }
        if (!source_copy_bytes(ctx, path, &out->literal_payload, &bits, sizeof(bits))) return false;
        return source_query_expression(ctx,path,type);
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
        if (source_text_size(ctx, path->as.member_access.name)==d->variants[v].name.length &&
            source_span_same(ctx, NULL, path->as.member_access.name, d->variants[v].name.bytes, d->variants[v].name.length)) break;
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
            if (source_text_size(ctx, name)==spelling.length && source_span_same(ctx, NULL, name, spelling.bytes, spelling.length)) break;
        }
        if (field==variant.field_count || out->fields[field])
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"unknown or duplicate pattern field");
        XrNameSpan span=payload->field_name_spans[i];
        XrXirSourceRange range={ctx->module,span.line,span.column,span.line,span.column};
        if (span.column>0 && source_text_size(ctx, name)<=(size_t)(INT_MAX-span.column)) range.end_column+=(int)source_text_size(ctx, name);
        if (!source_query_target_reference(ctx,range,ctx->nominal_members[declaration][variant.field_begin+field],XR_XIR_SOURCE_READ)) return false;
        SourceMatchPattern *child=source_alloc(ctx,1,sizeof(*child));
        if (!child || !source_match_pattern(ctx,payload->patterns[i],types[field],child,depth+1)) return false;
        out->fields[field]=child;
    }
    return true;
}
