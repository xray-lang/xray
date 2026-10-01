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
    CallExprNode *call = &node->as.call_expr;
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
    if (!source_argument_arity(ctx,node,index,supplied)) return false;
    if (call->type_arg_count && ((uint32_t)call->type_arg_count != own || !call->type_args))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"call requires its exact explicit type arguments or an omitted list");
    SourceCallStorage storage={0};
    if (!source_call_storage(ctx,count,function.parameter_count,&storage)) return false;
    XrXirType *types=storage.types;
    SourceValue *values=storage.values;
    for (uint32_t p=0;p<prefix;++p) {
        if (!source_work(ctx,node)) return false;
        types[p]=request->prefix.types[p];
    }
    if (offset) values[0]=*request->receiver;
    bool ok = false;
    SourceSubstitution substitution = {types,count};
    SourceCallPlan plan = {SOURCE_CALL_FUNCTION,request->prefix,substitution,function.parameters,NULL,
        function.result,request->expected_result,values,offset,offset,ctx->generics[index].parameter_kinds};
    if (!source_call_plan_arguments(ctx,node,&plan)) return false;
    if (!source_instantiation_prove(ctx,node,(XrXirDeclarationContext){XR_XIR_CONTEXT_FUNCTION,index,0},substitution)) goto done;
    for (uint32_t p=supplied;p<function.parameter_count;++p)
        if (!source_argument_default(ctx,node,index,p,&substitution,&values[p])) goto done;
    SourceCallConversions conversions={SOURCE_CALL_FUNCTION,substitution,function.parameters,NULL,
        values,function.parameter_count,0};
    if (!source_call_conversions(ctx,node,&conversions)) goto done;
    XrXirType result;
    if (!source_substitute(ctx,&substitution,function.result,0,&result)) goto done;
    *output=(SourceDirectArguments){substitution,values,function.parameter_count,result}; ok=true;
done:
    return ok;
}
