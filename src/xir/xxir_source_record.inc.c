/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_record.inc.c - Source-order record values with exact structural field projection
 */
static bool source_record_literal(SourceContext *ctx,AstNode *node,SourceExpectedType expected,SourceValue *out) {
    ObjectLiteralNode *literal=&node->as.object_literal;
    if(literal->count<0||literal->count>65536||(literal->count&&(!literal->keys||!literal->values)))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"record literal fields are malformed");
    uint32_t count=(uint32_t)literal->count;
    XrXirLiteral *names=count?source_alloc(ctx,count,sizeof(*names)):NULL;
    XrXirType *types=count?source_alloc(ctx,count,sizeof(*types)):NULL;
    SourceValue *values=count?source_alloc(ctx,count,sizeof(*values)):NULL;
    if(count&&(!names||!types||!values))return false;
    XrXirType context=expected.type;
    if(expected.present&&xr_xir_type_is_nullable(&ctx->types,context))context=xr_xir_nullable_element(&ctx->types,context);
    bool has_context=expected.present&&source_record_kind(ctx,context);
    uint32_t declaration=has_context?xr_xir_type_node(&ctx->types,context)->nominal.declaration:UINT32_MAX;
    for(uint32_t f=0;f<count;++f) {
        AstNode *key=literal->keys[f];
        if(!source_request_poll(ctx,key))return false;
        if(!key||key->type!=AST_LITERAL_STRING)return source_fail(ctx,node,XR_XIR_BAD_TYPE,"record spread is not admitted");
        if(key->as.literal.string_length>UINT32_MAX)return source_fail(ctx,key,XR_XIR_BUDGET,"record field name exceeds its metadata bound");
        names[f]=(XrXirLiteral){key->as.literal.raw_value.string_val,(uint32_t)key->as.literal.string_length};
        for(uint32_t p=0;p<f;++p)if(names[p].length==names[f].length&&
            source_span_same(ctx,key,names[p].bytes,names[f].bytes,names[f].length))return source_fail(ctx,key,XR_XIR_BAD_TYPE,"duplicate record field");
        SourceExpectedType field={0};
        if(declaration!=UINT32_MAX) {
            XrXirNominalDeclaration shape=ctx->nominals.declarations[declaration];
            for(uint32_t k=0;k<shape.field_count;++k) {
                if(!source_request_poll(ctx,key))return false;
                if(shape.fields[k].name.length==names[f].length&&source_span_same(ctx,key,shape.fields[k].name.bytes,names[f].bytes,names[f].length)) {
                    field=(SourceExpectedType){true,xr_xir_type_node(&ctx->types,context)->nominal.arguments[k],false,false};break;
                }
            }
            if(!field.present)return source_fail(ctx,key,XR_XIR_BAD_TYPE,"record field is outside its exact shape");
        }
        if(!source_plan_expression(ctx,literal->values[f],field,&values[f]))return false;
        types[f]=values[f].type;
    }
    XrXirType type;if(!source_record_type(ctx,node,names,types,count,&type))return false;
    if(has_context&&type!=context)return source_fail(ctx,node,XR_XIR_BAD_TYPE,"record literal requires its exact contextual shape");
    uint32_t *order=count?source_alloc(ctx,count,sizeof(*order)):NULL;
    SourceValue *ordered=count?source_alloc(ctx,count,sizeof(*ordered)):NULL;
    if(count&&(!order||!ordered))return false;
    if(!source_record_order(ctx,node,names,count,order))return false;
    for(uint32_t f=0;f<count;++f)ordered[f]=values[order[f]];
    return source_hir_nominal_construct(ctx,node,&(SourceNominalEmission){type,0,ordered,count},out);
}
