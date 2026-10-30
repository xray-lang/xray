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
static bool source_pattern_product_select(SourceContext *ctx,AstNode *node,
    SourcePatternProduct *product,const char *name,uint32_t *output) {
    size_t length=0;if (!source_text_length(ctx,node,name,&length)) return false;
    if(product->shape.owner) {
        if(length>UINT32_MAX)return source_fail(ctx,node,XR_XIR_BUDGET,"product field name exhausted");
        XrXirStatus status=xr_hir_body_nominal_find(ctx->typed_body,product->shape,(XrXirLiteral){name,(uint32_t)length},ctx->request,output,NULL);
        return source_hir_nominal_status(ctx,node,status,"unknown product pattern field");
    }
    for (uint32_t f=0;f<product->count;++f) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        XrXirLiteral spelling=product->fields[f].name;
        if (length==spelling.length && source_span_same(ctx,node,name,spelling.bytes,length)) {*output=f;return true;}
    }
    return source_fail(ctx,node,XR_XIR_BAD_TYPE,"unknown product pattern field");
}
static bool source_pattern_product_access(SourceContext *ctx,AstNode *node,
    XrXirType type,const char *name,XrNameSpan span,SourceProductField field) {
    AstNode selection={0};AstNode *site=node;
    if (span.line>0) {
        selection.type=AST_MEMBER_ACCESS;selection.line=span.line;selection.column=span.column;
        selection.end_line=span.line;selection.end_column=span.column+(int)source_text_size(ctx,name);
        selection.as.member_access.name=(char *)name;selection.as.member_access.name_span=span;site=&selection;
    }
    uint32_t index=0;XrXirType actual;
    return source_struct_field(ctx,site,type,name,false,&index,&actual) &&
        ((index==field.selection && actual==field.type) ||
        source_fail(ctx,site,XR_XIR_BAD_TYPE,"product field differs from its declaration"));
}
static bool source_pattern_product(SourceContext *ctx,AstNode *node,XrXirType type,
    SourceMatchPattern *out,uint32_t depth) {
    bool tuple=node->type==AST_PATTERN_TUPLE;
    int count=tuple ? node->as.pattern_tuple.count : node->as.pattern_object.count;
    AstNode **patterns=tuple ? node->as.pattern_tuple.patterns : node->as.pattern_object.patterns;
    if (count<0 || (count && (!patterns || (!tuple && !node->as.pattern_object.field_names))))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"product pattern children are missing");
    XrXirOp operation=source_pattern_product_kind(ctx,type);
    if ((tuple && operation!=XR_XIR_TUPLE_FIELD) || (!tuple && (operation==XR_XIR_TUPLE_FIELD || operation==XR_XIR_OP_COUNT)))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"product pattern differs from its scrutinee");
    if (!source_pattern_product_description(ctx,type,&out->product)) return false;
    SourcePatternProduct *product=out->product;out->count=product->count;
    if(tuple) {
        XrXirStatus status=xr_hir_tuple_arity(&ctx->types,type,(uint32_t)count,ctx->request,NULL);
        if(status!=XR_XIR_OK)return source_fail(ctx,node,status,"Tuple pattern requires its exact ordered arity");
    }
    out->fields=out->count ? source_alloc(ctx,out->count,sizeof(*out->fields)) : NULL;
    if (out->count && !out->fields) return false;
    for (int i=0;i<count;++i) {
        uint32_t f=(uint32_t)i;
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (!tuple) {
            const char *name=node->as.pattern_object.field_names[i];
            if (!source_pattern_product_select(ctx,node,product,name,&f)) return false;
            XrNameSpan span=node->as.pattern_object.field_name_spans ? node->as.pattern_object.field_name_spans[i] : (XrNameSpan){0};
            if (!source_pattern_product_access(ctx,node,type,name,span,product->fields[f])) return false;
        }
        if (out->fields[f]) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"duplicate product pattern field");
        SourceMatchPattern *child=source_alloc(ctx,1,sizeof(*child));
        if (!child || !source_match_pattern(ctx,patterns[i],product->fields[f].type,child,depth+1)) return false;
        if (!tuple && child->binding && node->as.pattern_object.field_name_spans) {
            XrNameSpan span=node->as.pattern_object.field_name_spans[i];
            if (child->binding->line==span.line && child->binding->column==span.column) {
                uint32_t owner=xr_xir_type_node(&ctx->types,type)->nominal.declaration;
                child->field_declaration=ctx->nominal_members[owner][f];
            }
        }
        out->fields[f]=child;
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
