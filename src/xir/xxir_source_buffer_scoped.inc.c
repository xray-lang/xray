/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_buffer_scoped.inc.c - Explicit calls within a genuine Buffer loan
 *
 * KEY CONCEPT:
 *   Argument completion precedes the loan. The callback remains an ordinary
 *   typed call, so its errors, suspension and root obligations stay visible.
 */
static bool source_buffer_scoped_call(SourceContext *ctx,AstNode *node,
    const SourceName *module,SourceValue *output) {
    if (!module || module->kind!=SOURCE_MODULE || !source_mem_module(ctx,module->module))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Buffer scope requires the canonical memory module");
    if (!ctx->bodies[ctx->function].unsafe_depth)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Buffer pointer scope requires unsafe");
    SourceCallSyntax syntax;
    if (!source_call_syntax(ctx,node,&syntax)) return false;
    if (syntax.arg_count!=4 || syntax.type_arg_count!=1 || !syntax.type_args ||
        (syntax.arg_accesses && (syntax.arg_accesses[0]!=XR_CALL_ARG_PLAIN ||
            syntax.arg_accesses[1]!=XR_CALL_ARG_PLAIN || syntax.arg_accesses[2]!=XR_CALL_ARG_REF ||
            syntax.arg_accesses[3]!=XR_CALL_ARG_PLAIN)))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Buffer scope requires a byte pointer, length, ref owner and callback");
    XrXirType element=XR_XIR_UNIT;
    if (!source_type(ctx,syntax.type_args[0],&element)) return false;
    if (element!=XR_XIR_U8)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Buffer scope currently requires u8");
    SourceName *begin=find_name(ctx,ctx->names[module->module],"__bufferSliceMutBegin");
    if (!begin || begin->kind!=SOURCE_FUNCTION || begin->index>=ctx->function_count ||
        ctx->identities[begin->index].native_callable!=XR_NATIVE_CALLABLE_BUFFER_SLICE_MUT_BEGIN)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Buffer scope has no actual begin declaration");
    uint32_t function=begin->index;XrXirType result=ctx->functions[function].result;
    const XrXirType *parameters=ctx->functions[function].parameters;
    SourceValue arguments[3]={{0}};
    SourceCallPlan plan={.family=SOURCE_CALL_FUNCTION,.function_parameters=parameters,
        .result=result,.values=arguments};
    plan.declaration.function=function;
    SourceCallSyntax prefix=syntax;prefix.arg_count=3;prefix.type_args=NULL;prefix.type_arg_count=0;
    if (!source_call_expand(ctx,node,&prefix,&plan,3) ||
        !source_call_plan_arguments(ctx,node,&plan,&prefix,NULL)) return false;
    SourceValue callback={0};
    if (!expression(ctx,syntax.arguments[3],&callback)) return false;
    const XrXirTypeNode *found=xr_xir_callable_signature(&ctx->types,callback.type);
    if (!found || found->parameter_count!=1 || !found->parameters ||
        found->parameters[0].mode!=XR_PARAM_READ || found->parameters[0].type!=result ||
        found->result!=XR_XIR_UNIT || (found->flags&XR_XIR_CALLABLE_REST))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Buffer callback requires one ordinary mutable byte view");
    XrXirTypeNode signature=*found;
    SourceValue view={0};
    if (!source_hir_call_commit(ctx,node,&plan,arguments,3,3) ||
        !source_hir_call_group(ctx,plan.signature,(XrXirInstruction){.op=XR_XIR_CALL,.type=result,
            .immediate=function},arguments,&view)) return false;
    SourceCallPlan invoke={.family=SOURCE_CALL_INDIRECT,.function_parameters=&result,
        .result=signature.result,.values=&view};
    invoke.declaration.callable=callback.type;invoke.declaration.callee_value=callback.id;
    if (!source_hir_call_prepare(ctx,node,&invoke,1) ||
        !source_hir_call_use(ctx,node,&invoke,(XrHirCallUseInput){.kind=XR_HIR_CALL_PLAN,.formal=0},NULL,NULL) ||
        !source_hir_call_use(ctx,node,&invoke,(XrHirCallUseInput){.kind=XR_HIR_CALL_ACCESS,.formal=0,.argument=0,
            .access=XR_CALL_ARG_PLAIN},NULL,NULL) ||
        !source_hir_call_use(ctx,node,&invoke,(XrHirCallUseInput){.kind=XR_HIR_CALL_VALUE,.formal=0,.argument=0},&view,NULL) ||
        !source_hir_call_commit(ctx,node,&invoke,&view,1,1) ||
        !source_hir_call_group(ctx,invoke.signature,(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,
            .type=signature.result,.immediate=callback.id},&view,output)) return false;
    return source_recipe_record(ctx,(XrXirInstruction){.op=XR_XIR_VIEW_END,.args={view.id}},NULL);
}
