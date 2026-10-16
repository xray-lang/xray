/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_direct_arguments.inc.c - Shared free and nominal method call operands
 *
 * KEY CONCEPT:
 *   Fixed owner arguments precede the sole own inference state; defaults execute
 *   only after explicit value evidence and complete declaration proof.
 */
#include "xxir_type_inference.h"
typedef struct SourceDirectRequest {
    uint32_t function;
    SourceSubstitution prefix;
    const SourceValue *receiver;
    SourceExpectedType expected_result;
} SourceDirectRequest;
typedef struct SourceDirectArguments {
    SourceSubstitution substitution;
    SourceValue *values;
    uint32_t count;
    XrXirType result;
} SourceDirectArguments;
static bool source_direct_arguments(SourceContext *ctx, AstNode *node,
    const SourceDirectRequest *request, SourceDirectArguments *output) {
    *output = (SourceDirectArguments){0};
    SourceCallSyntax syntax;if (!source_call_syntax(ctx,node,&syntax)) return false;
    const SourceCallSyntax *call=&syntax;
    uint32_t index = request->function, offset = request->receiver ? 1 : 0;
    XrXirFunction function = ctx->functions[index];
    uint32_t count = ctx->generics[index].parameter_count, prefix = request->prefix.count;
    if (prefix > count || count > 65536 || call->arg_count < 0 || call->type_arg_count < 0 ||
        (call->arg_count && !call->arguments) || (prefix && !request->prefix.types))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"direct call declaration shape mismatch");
    const XrXirFunctionIdentity *identity = &ctx->identities[index];
    uint32_t declared_prefix = identity->nominal_owner ?
        ctx->nominals.declarations[identity->nominal_owner-1].parameter_count : 0;
    if (prefix != declared_prefix || (!!request->receiver != (identity->method_kind == XR_XIR_READ_METHOD)))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"direct call parameter owner mismatch");
    uint32_t own = count-prefix, supplied = (uint32_t)call->arg_count+offset;
    if (call->type_arg_count && ctx->generics[index].parameter_kinds)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"callback result variables require inferred callable evidence");
    if (call->type_arg_count && ((uint32_t)call->type_arg_count != own || !call->type_args))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"call requires its exact explicit type arguments or an omitted list");
    SourceCallStorage storage={0};
    if (!source_call_storage(ctx,count,0,&storage)) return false;
    XrXirType *types=storage.types;
    for (uint32_t p=0;p<prefix;++p) {
        if (!source_work(ctx,node)) return false;
        types[p]=request->prefix.types[p];
    }
    bool ok = false;
    SourceSubstitution substitution = {types,count};
    SourceCallPlan plan = {SOURCE_CALL_FUNCTION,request->prefix,substitution,function.parameters,NULL,
        function.result,request->expected_result,NULL,offset,offset,ctx->generics[index].parameter_kinds,NULL,0,0};
    if (!source_call_rest_shape(ctx,node,index,&plan) ||
        !source_call_expand(ctx,node,&syntax,&plan,function.parameter_count)) return false;
    supplied=(uint32_t)syntax.arg_count+offset;
    if (!source_argument_arity(ctx,node,index,supplied)) return false;
    uint32_t capacity=supplied>function.parameter_count ? supplied : function.parameter_count;
    SourceValue *values=capacity ? source_recipe_storage(ctx,capacity,sizeof(*values)) : NULL;
    if (capacity && !values) return false;
    plan.values=values;
    if (offset) values[0]=*request->receiver;
    if (!source_call_plan_arguments(ctx,node,&plan,&syntax)) return false;
    if (!source_instantiation_prove(ctx,node,(XrXirDeclarationContext){XR_XIR_CONTEXT_FUNCTION,index,0},substitution)) goto done;
    uint32_t fixed=function.parameter_count-(plan.rest_parameter!=0);
    for (uint32_t p=supplied;p<fixed;++p)
        if (!source_argument_default(ctx,node,index,p,&substitution,&values[p])) goto done;
    SourceCallConversions conversions={SOURCE_CALL_FUNCTION,substitution,function.parameters,NULL,
        values,supplied>fixed ? supplied : fixed,0,plan.rest_parameter,plan.rest_element};
    if (!source_call_conversions(ctx,node,&conversions) || !source_call_rest_pack(ctx,node,&plan,supplied)) goto done;
    XrXirType result;
    if (!source_substitute(ctx,&substitution,function.result,0,&result)) goto done;
    *output=(SourceDirectArguments){substitution,values,function.parameter_count,result}; ok=true;
done:
    return ok;
}
