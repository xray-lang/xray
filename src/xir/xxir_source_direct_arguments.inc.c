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
    uint32_t receiver_count;
    XrHirBodySignature signature;
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
    const XrXirConstraint *constraints=ctx->generics[index].constraints;
    if (own && !constraints) return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"direct call binder is missing");
    const XrXirConstraint *own_constraints=own ? constraints+prefix : NULL;
    if (call->type_arg_count && ctx->generics[index].parameter_kinds)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"callback result variables require inferred callable evidence");
    if (call->type_arg_count && ((uint32_t)call->type_arg_count != own || !call->type_args))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"call requires its exact explicit type arguments or an omitted list");
    SourceCallStorage storage={0};
    if (!source_call_storage(ctx,count,function.parameter_count,&storage)) return false;
    XrXirType *types=storage.types;
    SourceValue *values=storage.values;
    for (uint32_t p=0;p<prefix;++p) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        types[p]=request->prefix.types[p];
    }
    if (offset) values[0]=*request->receiver;
    bool ok = false;
    SourceSubstitution substitution = {types,count};
    SourceCallPlan plan = {.family=SOURCE_CALL_FUNCTION,.prefix=request->prefix,.substitution=substitution,.function_parameters=function.parameters,.requirement_parameters=NULL,
        .result=function.result,.expected_result=request->expected_result,.values=values,.parameter_offset=offset,.value_offset=offset,.parameter_kinds=ctx->generics[index].parameter_kinds,.argument_contexts=NULL};
    plan.declaration.function=index;
    const XrXirTypeNode *task=xr_xir_type_node(&ctx->types,function.result);
    if (!plan.expected_result.present && ctx->active_expression &&
        ctx->active_expression->task_element_context.present && task && task->kind==XR_XIR_TYPE_TASK) {
        /* Await contributes result-element evidence only. It says nothing
         * about the error parameter of the returned Task handle. */
        plan.result=task->element;plan.expected_result=ctx->active_expression->task_element_context;
    }
    if (!source_call_expand(ctx,node,&syntax,&plan,function.parameter_count)) return false;
    supplied=(uint32_t)syntax.arg_count+offset;
    if (!source_argument_arity(ctx,node,index,supplied) || !source_call_plan_arguments(ctx,node,&plan,&syntax,own_constraints)) return false;
    if (!source_instantiation_prove(ctx,node,(XrXirDeclarationContext){XR_XIR_CONTEXT_FUNCTION,index,0},substitution)) goto done;
    for (uint32_t p=supplied;p<function.parameter_count;++p)
        if (!source_argument_default(ctx,node,index,p,&substitution,&values[p])) goto done;
    SourceCallConversions conversions={SOURCE_CALL_FUNCTION,substitution,function.parameters,NULL,
        values,function.parameter_count,0};
    if (!source_call_conversions(ctx,node,&conversions)) goto done;
    XrXirType result;
    if (!source_substitute(ctx,&substitution,function.result,0,&result)) goto done;
    if(!source_hir_call_commit(ctx,node,&plan,values,function.parameter_count,supplied))goto done;
    *output=(SourceDirectArguments){substitution,values,function.parameter_count,result,offset,plan.signature}; ok=true;
done:
    return ok;
}

static bool source_direct_signature_names(SourceContext *ctx,AstNode *node,XrXirSourceCallSignature *signature) {
    uint32_t owner=signature->owner_type.generic_owner;
    if(owner&&signature->result.generic_owner&&owner!=signature->result.generic_owner)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"call signature result owner diverged");
    if(signature->result.generic_owner)owner=signature->result.generic_owner;
    for(uint32_t p=0;p<signature->parameter_count;++p) {
        if(!((ctx)->diagnostic.status == XR_XIR_OK))return false;
        uint32_t candidate=signature->parameters[p].generic_owner;
        if(owner&&candidate&&owner!=candidate)
            return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"call signature generic owners diverged");
        if(candidate)owner=candidate;
    }
    if(!owner)return true;
    SourceFunction body=ctx->bodies[ctx->function];
    uint32_t count=ctx->type_scope.active?ctx->type_scope.count:body.type_parameter_count;
    uint32_t actual=ctx->type_scope.active?ctx->type_scope.generic_owner:body.generic_owner;
    XrGenericParam **parameters=ctx->type_scope.active?ctx->type_scope.parameters:body.type_parameters;
    if(owner!=actual||owner>ctx->query.declaration_count||!parameters||!count||
        count!=ctx->query.declarations[owner-1].generic_parameter_count)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"call signature generic names have no owner");
    const char **names=source_alloc(ctx,count,sizeof(*names));if(!names)return false;
    for(uint32_t p=0;p<count;++p) {
        size_t length=0;
        if(!parameters[p])return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"call signature generic parameter is missing");
        if(!source_text_length(ctx,node,parameters[p]->name,&length))return false;
        if(!length)return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"call signature generic name is empty");
        char *name=source_alloc(ctx,length+1,1);
        if(!name||!source_copy_bytes(ctx,node,name,parameters[p]->name,length+1))return false;
        names[p]=name;
    }
    signature->type_parameters=names;signature->type_parameter_count=count;signature->type_parameter_owner=owner;
    return true;
}

/* The CALL reference is appended only after nested arguments have finished.
 * Retain its ordinal, never an array element pointer across metadata growth. */
static bool source_direct_call_signature(SourceContext *ctx,AstNode *node,uint32_t function,
    const SourceDirectArguments *prepared,uint32_t reference_before) {
    if(ctx->query.reference_count==reference_before)return true;
    if(reference_before==UINT32_MAX||ctx->query.reference_count!=reference_before+1||!ctx->calls.count||
        ctx->calls.items[ctx->calls.count-1].reference!=reference_before||function>=ctx->function_count)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"direct call observation identity diverged");
    XrXirFunction declaration=ctx->functions[function];
    if(prepared->count!=declaration.parameter_count||prepared->receiver_count>prepared->count)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"direct call signature arity diverged");
    XrXirSourceType *parameters=prepared->count?source_alloc(ctx,prepared->count,sizeof(*parameters)):NULL;
    if(prepared->count&&!parameters)return false;
    for(uint32_t p=0;p<prepared->count;++p) {
        if(!((ctx)->diagnostic.status == XR_XIR_OK))return false;
        XrXirType shown;
        if(!source_substitute(ctx,&prepared->substitution,declaration.parameters[p],0,&shown))return false;
        if(xr_xir_type_is_cell(&ctx->types,shown))shown=xr_xir_cell_element(&ctx->types,shown);
        parameters[p]=source_query_type(ctx,shown);
    }
    XrXirSourceCallSignature signature={.parameters=parameters,.parameter_count=prepared->count,
        .receiver_count=prepared->receiver_count,.result=source_query_type(ctx,prepared->result),.known=true};
    uint32_t declaration_id=ctx->bodies[function].declaration;
    uint32_t owner=declaration_id?ctx->query.declarations[declaration_id-1].parent:0;
    if(owner&&ctx->query.declarations[owner-1].kind==XR_XIR_SOURCE_TYPE) {
        XrXirType actual;
        if(!ctx->query.declarations[owner-1].type.known)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"method signature owner type is unknown");
        if(!source_substitute(ctx,&prepared->substitution,ctx->query.declarations[owner-1].type.type,0,&actual))return false;
        signature.owner_type=source_query_type(ctx,actual);
    }
    if(!source_direct_signature_names(ctx,node,&signature))return false;
    XrXirSourceCall *call=(XrXirSourceCall *)&ctx->calls.items[ctx->calls.count-1];
    return source_copy_bytes(ctx,node,&call->signature,&signature,sizeof(signature));
}
