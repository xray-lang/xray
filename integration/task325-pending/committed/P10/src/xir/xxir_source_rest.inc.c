/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_rest.inc.c - Declaration-owned rest operands and Array packing
 *
 * KEY CONCEPT:
 *   Logical READ operands complete before one ordinary Array becomes the last
 *   physical argument. No syntax or borrowed argument vector crosses the call.
 */
static bool source_rest_parameter(SourceContext *ctx,AstNode *node,const XrParamNode *parameter,bool last,XrXirType *type) {
    if (!parameter->is_rest) return true;
    if (!last || parameter->passing_mode!=XR_PARAM_READ || parameter->default_value || !parameter->type)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"one last READ rest parameter requires an explicit element type and no default");
    return source_array_element_type(ctx,*type,type);
}
static bool source_call_rest_shape(SourceContext *ctx,AstNode *node,uint32_t function,SourceCallPlan *plan) {
    if (!ctx->identities[function].variadic) return true;
    uint32_t count=ctx->functions[function].parameter_count;
    if (!count || !xr_xir_type_is_array(&ctx->types,plan->function_parameters[count-1]))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"rest declaration requires a last Array parameter");
    plan->rest_parameter=count;
    plan->rest_element=xr_xir_array_element(&ctx->types,plan->function_parameters[count-1]);
    return true;
}
static bool source_call_rest_pack(SourceContext *ctx,AstNode *node,const SourceCallPlan *plan,uint32_t supplied) {
    if (!plan->rest_parameter) return true;
    uint32_t fixed=plan->rest_parameter-1,tail=supplied>fixed ? supplied-fixed : 0;
    XrXirType array=plan->function_parameters[fixed];
    if (!source_call_caller_parameters(plan) && !source_substitute(ctx,&plan->substitution,array,0,&array)) return false;
    if (!source_work(ctx,node)) return false;
    SourceValue packed;
    if (!source_recipe_group(ctx,(XrXirInstruction){XR_XIR_ARRAY_NEW,array,{0},{0},0,{0}},
            tail ? plan->values+fixed : NULL,tail,&packed)) return false;
    plan->values[fixed]=packed;
    return true;
}
