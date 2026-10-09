/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_tuple.inc.c - Ordered evaluation with a non-Unit operand table
 */
static bool source_tuple_index(SourceContext *ctx,AstNode *node,const char *name,uint32_t *field) {
    size_t length=0;uint32_t index=0;
    if (!source_text_length(ctx,node,name,&length)) return false;
    if (!length) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Tuple field index is empty");
    for (size_t i=0;i<length;++i) {
        if (!source_work(ctx,node)) return false;
        unsigned char digit=(unsigned char)name[i];
        if (digit<'0' || digit>'9' || index>(UINT32_MAX-(digit-'0'))/10)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Tuple field requires a bounded decimal index");
        index=index*10+(digit-'0');
    }
    *field=index;return true;
}
static bool source_tuple_literal(SourceContext *ctx,AstNode *node,SourceExpectedType expected,SourceValue *value) {
    const TupleLiteralNode *literal=&node->as.tuple_literal;
    if (literal->count<0 || (literal->count && !literal->elements))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Tuple literal ordered fields are missing");
    if (!literal->count) {
        if (expected.present && expected.type!=XR_XIR_UNIT)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"empty Tuple syntax denotes Unit");
        *value=(SourceValue){UINT32_MAX,XR_XIR_UNIT};return true;
    }
    const XrXirTypeNode *context=expected.present ? xr_xir_tuple_signature(&ctx->types,expected.type) : NULL;
    uint32_t count=(uint32_t)literal->count;
    if (expected.present && (!context || context->parameter_count!=count))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Tuple literal requires an exact ordered arity context");
    XrXirCallableParameter *fields=source_alloc(ctx,count,sizeof(*fields));
    SourceValue *payloads=source_alloc(ctx,count,sizeof(*payloads));
    if (!fields || !payloads) return false;
    /* Child evaluation can grow the type pool. Keep its field context owned. */
    if (context && !source_copy_bytes(ctx,node,fields,context->parameters,count*sizeof(*fields))) return false;
    uint32_t payload_count=0;
    for (uint32_t i=0;i<count;++i) {
        AstNode *child=literal->elements[i];SourceValue element={0};
        if (!child || child->type==AST_SPREAD_EXPR)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Tuple spread requires its complete expansion contract");
        SourceExpectedType hint={expected.present,fields[i].type,false, expected.inferred};
        if (!source_plan_expression(ctx,child,hint,&element)) return false;
        if (expected.present && element.type!=fields[i].type)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Tuple field does not satisfy its ordered context");
        fields[i]=(XrXirCallableParameter){element.type,0};
        if (element.type!=XR_XIR_UNIT) payloads[payload_count++]=element;
    }
    XrXirType tuple=XR_XIR_UNIT;
    if (!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,
        .parameters=fields,.parameter_count=count},&tuple)) return false;
    return source_recipe_group(ctx,(XrXirInstruction){XR_XIR_TUPLE_NEW,tuple,{0},{0},0,{0}},
        payloads,payload_count,value);
}
static bool source_tuple_field(SourceContext *ctx,AstNode *node,SourceValue receiver,SourceValue *value) {
    uint32_t field=0;
    if (!source_tuple_index(ctx,node,node->as.member_access.name,&field)) return false;
    const XrXirTypeNode *tuple=xr_xir_tuple_signature(&ctx->types,receiver.type);
    if (!tuple || field>=tuple->parameter_count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Tuple field index exceeds its ordered arity");
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_TUPLE_FIELD,tuple->parameters[field].type,
        {receiver.id,0},{0},field,{0}},value);
}
