/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_pattern_product.inc.c - Exact product constructors and field selections
 */
typedef struct SourceProductField {
    XrXirLiteral name;
    XrXirType type;
    uint64_t selection;
} SourceProductField;
typedef struct SourcePatternProduct {
    SourceProductField *fields;
    uint32_t count;
    XrXirOp operation;
    XrHirNominalShape shape;
} SourcePatternProduct;
static XrXirOp source_pattern_product_kind(SourceContext *ctx,XrXirType type) {
    if (xr_hir_tuple_kind(&ctx->types,type)!=XR_HIR_TUPLE_OTHER) return XR_XIR_TUPLE_FIELD;
    if (xr_xir_type_is_struct(&ctx->types,type)) return XR_XIR_STRUCT_GET;
    if (xr_xir_type_is_class(&ctx->types,type)) return XR_XIR_CLASS_GET;
    return XR_XIR_OP_COUNT;
}
static bool source_pattern_product_description(SourceContext *ctx,XrXirType type,SourcePatternProduct **output) {
    SourcePatternProduct *product=source_alloc(ctx,1,sizeof(*product));
    if (!product) return false;
    product->operation=source_pattern_product_kind(ctx,type);
    if (product->operation==XR_XIR_OP_COUNT)
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"product pattern requires statically known fields");
    bool nominal=product->operation==XR_XIR_STRUCT_GET || product->operation==XR_XIR_CLASS_GET;
    XrHirNominalInfo info={0};
    XrHirTupleView tuple={0};
    if (nominal) {
        if(!source_hir_nominal_shape(ctx,NULL,type,&product->shape,&info))return false;
        product->count=info.count;
    } else {
        XrXirStatus status=xr_hir_tuple_view(&ctx->types,type,ctx->request,&tuple,NULL);
        if(status!=XR_XIR_OK)return source_fail(ctx,NULL,status,"product pattern requires statically known fields");
        product->count=tuple.count;
    }
    product->fields=product->count ? source_alloc(ctx,product->count,sizeof(*product->fields)) : NULL;
    if (product->count && !product->fields) return false;
    for (uint32_t f=0;f<product->count;++f) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        SourceProductField *field=&product->fields[f];field->selection=f;
        if (nominal) {
            XrHirNominalField received={0};if(!source_hir_nominal_field(ctx,NULL,product->shape,f,&received))return false;
            field->name=received.name;field->type=received.type;
        } else {
            field->type=tuple.fields[f].type;
        }
    }
    *output=product;return true;
}
static bool source_pattern_product(SourceContext *ctx,AstNode *node,XrXirType type,
    SourceMatchPattern *out,uint32_t depth) {
    if (node->type!=AST_PATTERN_TUPLE)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"tuple pattern requires its own syntax");
    int count=node->as.pattern_tuple.count;
    AstNode **patterns=node->as.pattern_tuple.patterns;
    if (count<0 || (count && !patterns))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"tuple pattern children are missing");
    XrXirStatus status=xr_hir_tuple_arity(&ctx->types,type,(uint32_t)count,ctx->request,NULL);
    if(status!=XR_XIR_OK)return source_fail(ctx,node,status,"Tuple pattern requires its exact ordered arity");
    if (!source_pattern_product_description(ctx,type,&out->product)) return false;
    SourcePatternProduct *product=out->product;out->count=product->count;
    out->fields=out->count ? source_alloc(ctx,out->count,sizeof(*out->fields)) : NULL;
    if (out->count && !out->fields) return false;
    for (int i=0;i<count;++i) {
        if (!source_request_poll(ctx,node)) return false;
        SourceMatchPattern *child=source_alloc(ctx,1,sizeof(*child));
        if (!child || !source_match_pattern(ctx,patterns[i],product->fields[i].type,child,depth+1)) return false;
        out->fields[i]=child;
    }
    return true;
}
static bool source_pattern_product_value(SourceContext *ctx,SourceMatchPattern *pattern,
    uint32_t field,SourceValue receiver,SourceValue *output) {
    SourceProductField selected=pattern->product->fields[field];
    if(pattern->product->operation==XR_XIR_TUPLE_FIELD)
        return source_tuple_projection(ctx,pattern->node,receiver,field,output);
    return source_hir_nominal_get(ctx,pattern->node,receiver,(uint32_t)selected.selection,0,output);
}
