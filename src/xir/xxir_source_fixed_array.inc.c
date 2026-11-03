/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_fixed_array.inc.c - Exact scalar lists and once-evaluated repeats
 *
 * KEY CONCEPT:
 *   A repeat duplicates one scalar value, not its evaluation or a dynamic Array.
 */
static bool source_fixed_array_literal(SourceContext *ctx,AstNode *node,
    SourceExpectedType expected,SourceValue *output) {
    ArrayLiteralNode *literal=&node->as.array_literal;
    XrXirType type=expected.present?expected.type:XR_XIR_UNIT;
    XrXirType element=expected.present?xir_fixed_array_element(&ctx->types,type):XR_XIR_UNIT;
    uint32_t count=expected.present?xr_xir_fixed_array_length(&ctx->types,type):0;
    if(expected.present && (!count || element==XR_XIR_UNIT))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"fixed literal requires a supported fixed array context");
    if(literal->is_repeat) {
        AstNode *length=literal->repeat_count;
        if(!length || length->type!=AST_LITERAL_INT || length->as.literal.int_overflows_i64 ||
            !length->as.literal.int_bits || length->as.literal.int_bits>65535)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"fixed repeat requires a bounded positive literal length");
        uint32_t repeated=(uint32_t)length->as.literal.int_bits;
        if(count && count!=repeated)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"fixed repeat length does not match its type");
        count=repeated;
    } else if(literal->count<0 || (uint32_t)literal->count!=count || !literal->elements) {
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"fixed literal length does not match its type");
    }
    SourceValue *elements=source_alloc(ctx,count,sizeof(*elements));if(!elements)return false;
    for(uint32_t i=0;i<count;++i) {
        if(!(i&63u) && !source_request_poll(ctx,node))return false;
        if(literal->is_repeat && i) {elements[i]=elements[0];continue;}
        AstNode *child=literal->is_repeat?literal->repeat_value:literal->elements[i];
        if(!child || child->type==AST_SPREAD_EXPR || !source_plan_expression(ctx,child,
            (SourceExpectedType){element!=XR_XIR_UNIT,element,false,false},&elements[i]))return false;
        if(element==XR_XIR_UNIT)element=elements[i].type;
        if(!xir_fixed_array_scalar_bytes(element) || elements[i].type!=element)
            return source_fail(ctx,child,XR_XIR_BAD_TYPE,"fixed literal requires one supported exact scalar type");
    }
    if(!expected.present && !source_fixed_type(ctx,node,element,count,&type))return false;
    return source_recipe_group(ctx,(XrXirInstruction){.op=XR_XIR_FIXED_ARRAY_NEW,.type=type},elements,count,output);
}
