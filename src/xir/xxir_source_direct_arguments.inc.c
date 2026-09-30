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
    if (!source_argument_arity(ctx,node,index,supplied)) return false;
    if (call->type_arg_count && ((uint32_t)call->type_arg_count != own || !call->type_args))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"call requires its exact explicit type arguments or an omitted list");
    XrXirType *types = count ? source_alloc(ctx,count,sizeof(*types)) : NULL;
    SourceValue *values = function.parameter_count ? source_alloc(ctx,function.parameter_count,sizeof(*values)) : NULL;
    if ((count && !types) || (function.parameter_count && !values)) return false;
    for (uint32_t p=0;p<prefix;++p) {
        if (!source_work(ctx,node)) return false;
        types[p]=request->prefix.types[p];
    }
    if (offset) values[0]=*request->receiver;
    bool inferred = own && !call->type_arg_count, ok = false;
    XrXirInferenceState *state = NULL;
    if (inferred) {
        XrXirInferenceRequest begin = {&ctx->types,request->prefix.types,prefix,own,
            ctx->generics[ctx->function].parameter_count};
        XrXirStatus status = xr_xir_inference_begin(&begin,&ctx->budget,&state);
        if (status != XR_XIR_OK) return source_fail(ctx,node,status,"direct call inference could not begin");
    } else for (uint32_t p=0;p<own;++p)
        if (!source_work(ctx,node) || !source_type(ctx,call->type_args[p],&types[prefix+p])) return false;
    SourceSubstitution substitution = {types,count};
    for (uint32_t p=offset;p<supplied;++p) {
        uint32_t argument=p-offset;
        if (call->arg_accesses && call->arg_accesses[argument] != XR_CALL_ARG_PLAIN) {
            source_fail(ctx,node,XR_XIR_BAD_TYPE,"direct call arguments must use the declared READ contract"); goto done;
        }
        XrXirType formal=function.parameters[p], expected=XR_XIR_UNIT;
        if (inferred) {
            XrXirInferenceKnown known={0};
            XrXirStatus status=xr_xir_inference_expected_known(state,&ctx->types,formal,&known);
            if (status != XR_XIR_OK) { source_fail(ctx,node,status,"direct call inference context is invalid"); goto done; }
            if (known.known) {
                SourceSubstitution partial={known.arguments,known.argument_count};
                if (!source_substitute(ctx,&partial,formal,0,&expected)) goto done;
            }
        } else if (!source_substitute(ctx,&substitution,formal,0,&expected)) goto done;
        if (!expression_in(ctx,call->arguments[argument],expected,&values[p])) goto done;
        if (values[p].type==XR_XIR_UNIT) { source_fail(ctx,node,XR_XIR_BAD_TYPE,"unit argument is not admitted"); goto done; }
        if (inferred) {
            XrXirType evidence=values[p].type;
            const XrXirTypeNode *wanted=xr_xir_callable_signature(&ctx->types,formal);
            const XrXirTypeNode *actual=xr_xir_callable_signature(&ctx->types,evidence);
            if (wanted && !wanted->flags && actual && actual->flags==XR_XIR_CALLABLE_NO_SUSPEND) {
                XrXirTypeNode ordinary=*actual; ordinary.flags=0;
                if (!source_intern_type(ctx,ordinary,&evidence)) goto done;
                XrXirStatus status=xr_xir_callable_weakening(&ctx->types,values[p].type,evidence,&ctx->budget.work);
                if (status != XR_XIR_OK) { source_fail(ctx,node,status,"callable inference evidence is invalid"); goto done; }
            }
            XrXirStatus status=xr_xir_inference_observe(state,&ctx->types,(XrXirInferencePair){formal,evidence});
            if (status != XR_XIR_OK) { source_fail(ctx,node,status,"direct call type evidence is inconsistent"); goto done; }
        }
    }
    if (inferred) {
        XrXirStatus status=xr_xir_inference_finalize(state,&ctx->types,types,count);
        if (status != XR_XIR_OK) { source_fail(ctx,node,status,"cannot infer all declaration type arguments; supply an explicit list"); goto done; }
        xr_xir_inference_dispose(state); state=NULL;
    }
    if (!source_instantiation_prove(ctx,node,(XrXirDeclarationContext){XR_XIR_CONTEXT_FUNCTION,index,0},substitution)) goto done;
    for (uint32_t p=supplied;p<function.parameter_count;++p)
        if (!source_argument_default(ctx,node,index,p,&substitution,&values[p])) goto done;
    for (uint32_t p=0;p<function.parameter_count;++p) {
        XrXirType expected;
        if (!source_substitute(ctx,&substitution,function.parameters[p],0,&expected) ||
            !source_expect(ctx,node,expected,&values[p])) goto done;
        if (values[p].type != expected) { source_fail(ctx,node,XR_XIR_BAD_TYPE,"direct argument type mismatch"); goto done; }
    }
    XrXirType result;
    if (!source_substitute(ctx,&substitution,function.result,0,&result)) goto done;
    *output=(SourceDirectArguments){substitution,values,function.parameter_count,result}; ok=true;
done:
    xr_xir_inference_dispose(state); return ok;
}
