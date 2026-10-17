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
} SourcePatternProduct;
static XrXirOp source_pattern_product_kind(SourceContext *ctx,XrXirType type) {
    if (type==XR_XIR_UNIT || xr_xir_tuple_signature(&ctx->types,type)) return XR_XIR_TUPLE_FIELD;
    if (xr_xir_type_is_record(&ctx->types,type)) return XR_XIR_RECORD_FIELD;
    if (xr_xir_type_is_struct(&ctx->types,type)) return XR_XIR_STRUCT_GET;
    if (xr_xir_type_is_class(&ctx->types,type)) return XR_XIR_CLASS_GET;
    uint32_t id=(uint32_t)type;
    if (id>=XR_XIR_TYPE_PARAMETER_BASE && id<XR_XIR_TYPE_PARAMETER_LIMIT) {
        const XrXirGeneric *generic=&ctx->generics[ctx->function];
        uint32_t p=id-XR_XIR_TYPE_PARAMETER_BASE;
        if (p<generic->parameter_count && generic->constraints && generic->constraints[p].structural_count)
            return XR_XIR_FIELD_REQUIREMENT;
    }
    return XR_XIR_OP_COUNT;
}
static bool source_pattern_product_insert(SourceContext *ctx,SourcePatternProduct *product,
    uint32_t *capacity,SourceProductField field) {
    uint32_t index=0;
    for (;index<product->count;++index) {
        int order=0;XrXirStatus status=xir_record_name_compare(&ctx->compile,field.name,product->fields[index].name,&order);
        if (status!=XR_XIR_OK) return source_fail(ctx,NULL,status,"product field name comparison failed");
        if (!order) {
            status=xr_xir_compile_type_substitution_matches(&ctx->compile,&ctx->types,NULL,0,
                field.type,product->fields[index].type);
            return status==XR_XIR_OK || source_fail(ctx,NULL,status,"product field constraints conflict");
        }
        if (order<0) break;
    }
    SourceProductField *fields=source_query_append(ctx,product->fields,&product->count,capacity,sizeof(*fields));
    if (!fields) return false;
    product->fields=fields;
    for (uint32_t i=product->count-1;i>index;--i) {
        if (!source_work(ctx,NULL)) return false;
        fields[i]=fields[i-1];
    }
    fields[index]=field;return true;
}
static bool source_pattern_product_description(SourceContext *ctx,XrXirType type,SourcePatternProduct **output) {
    SourcePatternProduct *product=source_alloc(ctx,1,sizeof(*product));
    if (!product) return false;
    product->operation=source_pattern_product_kind(ctx,type);
    if (product->operation==XR_XIR_OP_COUNT)
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"product pattern requires statically known fields");
    if (product->operation==XR_XIR_FIELD_REQUIREMENT) {
        const XrXirGeneric *generic=&ctx->generics[ctx->function];
        const XrXirConstraint *constraint=&generic->constraints[(uint32_t)type-XR_XIR_TYPE_PARAMETER_BASE];
        uint32_t capacity=0;
        for (uint32_t r=0;r<constraint->structural_count;++r) {
            if (!source_work(ctx,NULL)) return false;
            const XrXirStructuralRequirement *requirement=&constraint->structural_requirements[r];
            const XrXirTypeNode *shape=xr_xir_record_signature(&ctx->types,requirement->shape);
            if (!shape || requirement->policy!=XR_XIR_STRUCTURAL_READONLY_RECEIVER)
                return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"product field policy is not admitted");
            for (uint32_t f=0;f<shape->parameter_count;++f) {
                SourceProductField field={shape->record.names[f],shape->parameters[f].type,((uint64_t)r<<32)|f};
                if (!source_pattern_product_insert(ctx,product,&capacity,field)) return false;
            }
        }
        *output=product;return true;
    }
    const XrXirTypeNode *node=xr_xir_type_node(&ctx->types,type);
    bool nominal=product->operation==XR_XIR_STRUCT_GET || product->operation==XR_XIR_CLASS_GET;
    XrXirNominalType instance={0};XrXirNominalDeclaration declaration={0};
    if (nominal) {
        instance=node->nominal;declaration=ctx->nominals.declarations[instance.declaration];
        product->count=declaration.field_count;
    } else product->count=type==XR_XIR_UNIT ? 0 : node->parameter_count;
    product->fields=product->count ? source_alloc(ctx,product->count,sizeof(*product->fields)) : NULL;
    if (product->count && !product->fields) return false;
    SourceSubstitution substitution={instance.arguments,instance.argument_count};
    for (uint32_t f=0;f<product->count;++f) {
        if (!source_work(ctx,NULL)) return false;
        SourceProductField *field=&product->fields[f];field->selection=f;
        if (nominal) {
            field->name=declaration.fields[f].name;
            if (!source_substitute(ctx,&substitution,declaration.fields[f].type,0,&field->type)) return false;
        } else {
            field->type=node->parameters[f].type;
            if (product->operation==XR_XIR_RECORD_FIELD) field->name=node->record.names[f];
        }
    }
    *output=product;return true;
}
static bool source_pattern_product_select(SourceContext *ctx,AstNode *node,
    SourcePatternProduct *product,const char *name,uint32_t *output) {
    size_t length=0;if (!source_text_length(ctx,node,name,&length)) return false;
    for (uint32_t f=0;f<product->count;++f) {
        if (!source_work(ctx,node)) return false;
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
    SourceStructuralField requirement={0};bool found=false;
    if (!source_structural_field_select(ctx,site,type,name,&requirement,&found)) return false;
    if (found) return requirement.type==field.type ||
        source_fail(ctx,site,XR_XIR_BAD_TYPE,"product field requirement differs from its declaration");
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
    if (tuple && (uint32_t)count!=out->count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Tuple pattern requires its exact ordered arity");
    out->fields=out->count ? source_alloc(ctx,out->count,sizeof(*out->fields)) : NULL;
    if (out->count && !out->fields) return false;
    for (int i=0;i<count;++i) {
        uint32_t f=(uint32_t)i;
        if (!source_work(ctx,node)) return false;
        if (!tuple) {
            const char *name=node->as.pattern_object.field_names[i];
            if (!source_pattern_product_select(ctx,node,product,name,&f)) return false;
            XrNameSpan span=node->as.pattern_object.field_name_spans ? node->as.pattern_object.field_name_spans[i] : (XrNameSpan){0};
            if (!source_pattern_product_access(ctx,node,type,name,span,product->fields[f])) return false;
        }
        if (out->fields[f]) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"duplicate product pattern field");
        SourceMatchPattern *child=source_alloc(ctx,1,sizeof(*child));
        if (!child || !source_match_pattern(ctx,patterns[i],product->fields[f].type,child,depth+1)) return false;
        out->fields[f]=child;
    }
    return true;
}
static bool source_pattern_product_value(SourceContext *ctx,SourceMatchPattern *pattern,
    uint32_t field,SourceValue receiver,SourceValue *output) {
    SourceProductField selected=pattern->product->fields[field];
    if (selected.selection>INT64_MAX)
        return source_fail(ctx,pattern->node,XR_XIR_BUDGET,"product field selector exhausted");
    return source_recipe_record(ctx,(XrXirInstruction){pattern->product->operation,selected.type,
        {receiver.id,0},{0},(int64_t)selected.selection,{0}},output);
}
