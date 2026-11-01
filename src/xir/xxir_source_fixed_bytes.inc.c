/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_fixed_bytes.inc.c - Decoded bytes retain exact nonempty fixed types
 *
 * KEY CONCEPT:
 *   Literal bytes are real u8 operands; construction does not substitute an Array.
 */
static bool source_fixed_type(SourceContext *ctx,AstNode *node,uint32_t count,XrXirType *output) {
    if(!count || count>65535)
        return source_fail_feature(ctx,node,XR_XIR_BAD_TYPE,"this fixed byte length");
    return source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_FIXED_ARRAY,
        .element=XR_XIR_U8,.parameter_count=count},output);
}
static bool source_fixed_type_ref(SourceContext *ctx,XrTypeRef *ref,XrXirType *output) {
    if(ref->nchildren!=1 || !ref->children || !ref->children[0] || ctx->depth>=128 ||
        ref->fixed_length<=0 || ref->fixed_length>65535 ||
        (ref->fixed_length_expr && (ref->fixed_length_expr->type!=AST_LITERAL_INT ||
        ref->fixed_length_expr->as.literal.int_overflows_i64 ||
        ref->fixed_length_expr->as.literal.int_bits!=(uint64_t)ref->fixed_length)))
        return source_fail_feature(ctx,NULL,XR_XIR_BAD_TYPE,"this fixed array annotation");
    XrXirType element=XR_XIR_UNIT;++ctx->depth;
    bool valid=source_type(ctx,ref->children[0],&element);--ctx->depth;
    if(!valid)return false;
    if(element!=XR_XIR_U8)return source_fail_feature(ctx,NULL,XR_XIR_BAD_TYPE,"this fixed array element");
    return source_fixed_type(ctx,NULL,(uint32_t)ref->fixed_length,output);
}
static bool source_fixed_literal_type(SourceContext *ctx,AstNode *node,XrXirType *output) {
    const FixedBytesLiteralNode *literal=&node->as.fixed_bytes_literal;
    if((literal->payload_length && !literal->payload) || literal->payload_length>65535 ||
        (literal->append_nul && literal->payload_length==65535))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"fixed literal has invalid exact byte storage");
    uint32_t count=(uint32_t)literal->payload_length+(uint32_t)literal->append_nul;
    return source_fixed_type(ctx,node,count,output);
}
static bool source_fixed_literal(SourceContext *ctx,AstNode *node,SourceValue *output) {
    XrXirType type=XR_XIR_UNIT;
    if(!source_fixed_literal_type(ctx,node,&type))return false;
    const FixedBytesLiteralNode *literal=&node->as.fixed_bytes_literal;
    uint32_t count=xr_xir_fixed_array_length(&ctx->types,type);
    SourceValue *elements=source_alloc(ctx,count,sizeof(*elements));if(!elements)return false;
    for(uint32_t i=0;i<count;++i) {
        if(!(i&63u) && !source_request_poll(ctx,node))return false;
        uint8_t byte=i<literal->payload_length?literal->payload[i]:0;
        if(literal->append_nul && i<literal->payload_length && !byte)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"C fixed literal contains an interior NUL");
        if(!source_recipe_record(ctx,(XrXirInstruction){.op=XR_XIR_CONST_INT,
            .type=XR_XIR_U8,.immediate=byte},&elements[i]))return false;
    }
    return source_recipe_group(ctx,(XrXirInstruction){.op=XR_XIR_FIXED_ARRAY_NEW,.type=type},
        elements,count,output);
}
