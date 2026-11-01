/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_rest_arguments.inc.c - Ordinary rest values retain one Array owner
 */
static bool source_array_append_snapshot(SourceContext *,SourceValue,SourceValue,SourceValue);
static uint32_t source_call_rest_prefix(const SourceCallPlan *plan,const SourceCallSyntax *call) {
    uint32_t supplied=(uint32_t)call->arg_count-(plan->rest_array!=UINT32_MAX?1u:0u);
    return supplied<plan->rest_count-1?supplied:plan->rest_count-1;
}
static bool source_call_rest_pack(SourceContext *ctx,AstNode *node,const SourceCallPlan *plan,
    const SourceCallSyntax *call,SourceValue *physical) {
    uint32_t first=source_call_rest_prefix(plan,call),last=plan->rest_count-1;
    uint32_t count=(uint32_t)call->arg_count,ordinary=count-(plan->rest_array!=UINT32_MAX?1u:0u);
    if(first>ordinary||(ordinary<last&&first!=ordinary)||plan->value_offset||plan->parameter_offset)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"rest operand prefix is inconsistent");
    if(!source_copy_bytes(ctx,node,physical,plan->values,(size_t)first*sizeof(*physical)))return false;
    XrXirType array;
    if(source_call_caller_parameters(plan))array=plan->function_parameters[last];
    else if(!source_substitute(ctx,&plan->substitution,plan->function_parameters[last],0,&array))return false;
    XrXirType element=xr_xir_array_element(&ctx->types,array),validated;
    if(!xr_xir_type_is_array(&ctx->types,array)||!source_array_element_type(ctx,element,&validated)||validated!=array)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"rest elements require an admitted Array value type");
    for(uint32_t a=first;a<count;++a) {
        if(!source_request_poll(ctx,node))return false;
        XrXirType expected=a==plan->rest_array?array:element;
        XrHirConversion conversion;
        if(!source_conversion_plan(ctx,source_call_argument_syntax(call,a),plan->values[a].type,
            (SourceExpectedType){true,expected,false,false},&conversion)||
            !source_conversion_emit(ctx,&conversion,&plan->values[a]))return false;
        if(plan->values[a].type!=expected)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"rest argument differs from its declared element type");
    }
    SourceValue packed={0};
    if(!source_native_array_declaration(ctx)||!source_recipe_group(ctx,
        (XrXirInstruction){XR_XIR_ARRAY_NEW,array,{0},{0},0,{0}},ordinary>first?plan->values+first:NULL,ordinary-first,&packed))return false;
    if(plan->rest_array!=UINT32_MAX) {
        if(plan->rest_array+1!=count)
            return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"an Array expansion must be last");
        XrXirType cell;SourceValue store,place,zero;
        if(!source_cell_type(ctx,array,&cell)||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_NEW,cell,{packed.id},{0},0,{0}},&store)||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_PLACE,array,{store.id},{0},0,{0}},&place)||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},&zero)||
            !source_array_append_snapshot(ctx,place,plan->values[plan->rest_array],zero)||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_READ,array,{store.id},{0},0,{0}},&packed))return false;
    }
    if(!source_hir_call_use(ctx,node,plan,(XrHirCallUseInput){.kind=XR_HIR_CALL_ACCESS,
        .formal=last,.argument=last,.access=XR_CALL_ARG_PLAIN},NULL,NULL)||
        !source_hir_call_use(ctx,node,plan,(XrHirCallUseInput){.kind=XR_HIR_CALL_VALUE,
        .formal=last,.argument=last},&packed,NULL))return false;
    physical[last]=packed;return true;
}
