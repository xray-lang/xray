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
#include "xxir_source_fixed_count.inc.c"
static bool source_fixed_type(SourceContext *ctx,AstNode *node,XrXirType element,
    uint32_t count,XrXirType *output) {
    if(!count || count>65535)
        return source_fail_feature(ctx,node,XR_XIR_BAD_TYPE,"this fixed byte length");
    return source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_FIXED_ARRAY,
        .element=element,.parameter_count=count},output);
}
static bool source_fixed_type_ref(SourceContext *ctx,XrTypeRef *ref,XrXirType *output) {
    if(ref->nchildren!=1 || !ref->children || !ref->children[0] || ctx->depth>=128)
        return source_fail_feature(ctx,NULL,XR_XIR_BAD_TYPE,"this fixed array annotation");
    uint32_t count = (uint32_t)ref->fixed_length;
    if (ref->fixed_length_expr && !source_fixed_count(ctx, ref->fixed_length_expr, &count)) return false;
    XrXirType element=XR_XIR_UNIT;++ctx->depth;
    bool valid=source_type(ctx,ref->children[0],&element);--ctx->depth;
    if(!valid)return false;
    if(!xr_xir_fixed_array_element_admitted(element))
        return source_fail_feature(ctx,NULL,XR_XIR_BAD_TYPE,"this fixed array element");
    return source_fixed_type(ctx,NULL,element,count,output);
}
static bool source_fixed_literal_type(SourceContext *ctx,AstNode *node,XrXirType *output) {
    const FixedBytesLiteralNode *literal=&node->as.fixed_bytes_literal;
    if((literal->payload_length && !literal->payload) || literal->payload_length>65535 ||
        (literal->append_nul && literal->payload_length==65535))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"fixed literal has invalid exact byte storage");
    uint32_t count=(uint32_t)literal->payload_length+(uint32_t)literal->append_nul;
    return source_fixed_type(ctx,node,XR_XIR_U8,count,output);
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
