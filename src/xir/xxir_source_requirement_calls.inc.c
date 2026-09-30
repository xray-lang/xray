/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_requirement_calls.inc.c - Definition-bound abstract method calls
 *
 * KEY CONCEPT:
 *   Incremental own evidence preserves original requirement authority and
 *   evaluates each argument once before complete tuple proof and publication.
 */
#include "xxir_type_inference.h"
static bool source_reify_type(SourceContext *ctx, AstNode *site,
    const XrXirTypes *pool, XrXirType input, XrXirType *output) {
    if (!source_work(ctx,site)) return false;
    const XrXirTypeNode *found = xr_xir_type_node(pool,input);
    if (!found) { *output = input; return true; }
    if (ctx->depth >= 128) return source_fail(ctx,site,XR_XIR_BUDGET,"requirement type depth exhausted");
    ++ctx->depth;
    XrXirTypeNode node = *found;
    bool ok = true;
    if (node.kind == XR_XIR_TYPE_CALLABLE) {
        XrXirCallableParameter *parameters = node.parameter_count ? source_alloc(ctx,node.parameter_count,sizeof(*parameters)) : NULL;
        ok = !node.parameter_count || parameters;
        for (uint32_t p = 0; p < node.parameter_count && ok; ++p) {
            parameters[p].mode = node.parameters[p].mode;
            ok = source_reify_type(ctx,site,pool,node.parameters[p].type,&parameters[p].type);
        }
        node.parameters = parameters;
        if (ok) ok = source_reify_type(ctx,site,pool,node.result,&node.result);
    } else if (node.kind == XR_XIR_TYPE_NOMINAL) {
        XrXirType *arguments = node.nominal.argument_count ? source_alloc(ctx,node.nominal.argument_count,sizeof(*arguments)) : NULL;
        ok = !node.nominal.argument_count || arguments;
        for (uint32_t a = 0; a < node.nominal.argument_count && ok; ++a)
            ok = source_reify_type(ctx,site,pool,node.nominal.arguments[a],&arguments[a]);
        node.nominal.arguments = arguments; node.nominal.fields = NULL; node.nominal.field_count = 0;
    } else ok = source_reify_type(ctx,site,pool,node.element,&node.element);
    --ctx->depth;
    return ok && source_intern_type(ctx,node,output);
}
static bool source_reify_application(SourceContext *ctx, AstNode *site,
    const XrXirTypes *pool, const XrXirInterfaceApplication *input, XrXirInterfaceApplication *output) {
    XrXirType *arguments = input->argument_count ? source_alloc(ctx,input->argument_count,sizeof(*arguments)) : NULL;
    if (input->argument_count && !arguments) return false;
    for (uint32_t a = 0; a < input->argument_count; ++a)
        if (!source_reify_type(ctx,site,pool,input->arguments[a],&arguments[a])) return false;
    *output = (XrXirInterfaceApplication){input->declaration,arguments,input->argument_count};
    return true;
}
static bool source_requirement_prove(SourceContext *ctx, AstNode *node,
    uint32_t member, XrXirInterfaceApplication application, SourceSubstitution substitution,
    XrXirType *signature) {
    const XrXirInterfaceMethod *method = &ctx->interfaces.declarations[application.declaration].methods[member];
    uint32_t own = method->own_parameter_count, parent = application.argument_count, count = substitution.count;
    const XrXirType *types = substitution.types;
    if (parent > 65536 || own > 65536-parent || count != parent+own)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"requirement substitution shape mismatch");
    SourceSubstitution prefix = {types,parent};
    if (!source_instantiation_prove(ctx,node,
        (XrXirDeclarationContext){XR_XIR_CONTEXT_INTERFACE,application.declaration,0},prefix)) return false;
    XrXirDeclarations declarations;
    XrXirModule module = source_module_view(ctx,&declarations);
    XrXirProofContext context = {&module,{XR_XIR_CONTEXT_FUNCTION,ctx->function,0}};
    for (uint32_t p = 0; p < own; ++p) {
        if (!source_work(ctx,node)) return false;
        XrXirConstraintUse use = {&module,
            {XR_XIR_CONTEXT_INTERFACE_METHOD,application.declaration,member},parent+p,types,count};
        XrXirStatus status = xr_xir_constraints_prove(&context,&use,&ctx->budget);
        if (status == XR_XIR_OK) status = xr_xir_type_access(&module,ctx->function,types[parent+p],&ctx->budget);
        if (status != XR_XIR_OK)
            return source_fail(ctx,node,status,"method type argument does not prove the declared constraint");
    }
    return source_substitute(ctx,&substitution,method->signature,0,signature);
}
typedef struct SourceRequirementSelection {
    XrXirInterfaceApplication application;
    uint32_t member;
} SourceRequirementSelection;
static bool source_requirement_select(SourceContext *ctx, AstNode *node,
    SourceValue receiver, const char *name, SourceRequirementSelection *output) {
    uint32_t parameter = (uint32_t)receiver.type - XR_XIR_TYPE_PARAMETER_BASE;
    const XrXirGeneric *generic = &ctx->generics[ctx->function];
    if (parameter >= generic->parameter_count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"receiver has no declaration parameter context");
    const XrXirConstraint *constraint = &generic->constraints[parameter];
    if (!constraint->interface_count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"receiver has no declared interface requirements");
    XrXirTypes input = ctx->types;
    XrXirInterfaceClosure *closure = NULL;
    uint64_t scratch = ctx->budget.scratch_bytes;
    XrXirInterfaceClosureRoots roots = {&ctx->interfaces,&input,
        constraint->interfaces,constraint->interface_count,generic->parameter_count};
    XrXirStatus status = xr_xir_interface_closure_build(&roots,&ctx->budget,&closure);
    if (status != XR_XIR_OK) return source_fail(ctx,node,status,"interface requirements are invalid");
    scratch -= ctx->budget.scratch_bytes;
    bool ok = false;
    const XrXirInterfaceRequirement *selected = NULL;
    size_t length = strlen(name);
    for (uint32_t r = 0; r < xr_xir_interface_closure_requirement_count(closure); ++r) {
        const XrXirInterfaceRequirement *requirement = xr_xir_interface_closure_requirement(closure,r);
        if (!source_work(ctx,node)) goto done;
        if (length == requirement->name.length && !memcmp(name,requirement->name.bytes,length))
            if (!selected) selected = requirement;
    }
    if (!selected) { source_fail(ctx,node,XR_XIR_BAD_TYPE,"member is absent from declared interface requirements"); goto done; }
    if (!source_reify_application(ctx,node,xr_xir_interface_closure_types(closure),
        xr_xir_interface_closure_application(closure,selected->application),&output->application)) goto done;
    if (output->application.declaration != selected->origin_interface) {
        source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"requirement application origin mismatch"); goto done;
    }
    output->member = selected->member; ok = true;
done:
    xr_xir_interface_closure_free(closure);
    ctx->budget.scratch_bytes += scratch; return ok;
}
static bool source_requirement_call(SourceContext *ctx, AstNode *node,
    SourceValue receiver, SourceExpectedType result_context, SourceValue *value) {
    CallExprNode *call = &node->as.call_expr;
    if (call->arg_count < 0 || (call->arg_count && !call->arguments))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"interface method arguments are malformed");
    SourceRequirementSelection selected = {0};
    if (!source_requirement_select(ctx,node,receiver,call->callee->as.member_access.name,&selected)) return false;
    XrXirInterfaceApplication application = selected.application;
    bool ok = false;
    XrXirType signature;
    const XrXirInterfaceMethod *method = &ctx->interfaces.declarations[application.declaration].methods[selected.member];
    /* Copy the node before argument expressions append to the descriptor arena. */
    XrXirTypeNode formal = *xr_xir_callable_signature(&ctx->types,method->signature);
    uint32_t own = method->own_parameter_count, parent = application.argument_count;
    if (parent > 65536 || own > 65536-parent || call->type_arg_count < 0 ||
        (call->type_arg_count && ((uint32_t)call->type_arg_count != own || !call->type_args))) {
        source_fail(ctx,node,XR_XIR_BAD_TYPE,"interface method requires its exact explicit type arguments or an omitted list"); goto done;
    }
    if ((uint32_t)call->arg_count != formal.parameter_count) {
        source_fail(ctx,node,XR_XIR_BAD_TYPE,"interface method argument arity mismatch"); goto done;
    }
    uint32_t count = parent+own;
    SourceCallStorage storage={0};
    if (!source_call_storage(ctx,count,(uint64_t)formal.parameter_count+1,&storage)) goto done;
    XrXirType *types=storage.types;
    for (uint32_t p = 0; p < parent; ++p) {
        if (!source_work(ctx,node)) goto done;
        types[p] = application.arguments[p];
    }
    SourceSubstitution substitution = {types,count};
    SourceValue *arguments=storage.values;
    arguments[0] = receiver;
    SourceCallPlan plan = {SOURCE_CALL_REQUIREMENT,{application.arguments,parent},substitution,NULL,
        formal.parameters,formal.result,result_context,arguments,0,1};
    if (!source_call_plan_arguments(ctx,node,&plan)) goto done;
    if (!source_requirement_prove(ctx,node,selected.member,application,substitution,&signature)) goto done;
    XrXirTypeNode callable = *xr_xir_callable_signature(&ctx->types,signature);
    SourceCallConversions conversions={SOURCE_CALL_REQUIREMENT,substitution,NULL,callable.parameters,
        arguments,callable.parameter_count,1};
    if (!source_call_conversions(ctx,node,&conversions)) goto done;
    XrXirInstruction op = {XR_XIR_CALL_REQUIREMENT,callable.result,{0},
        {application.declaration,selected.member},0,{0}};
    ok = source_type_arguments(ctx,node,substitution.types,substitution.count,&op) &&
        source_query_target_reference(ctx,source_query_range(ctx,call->callee,NULL),
            ctx->interface_member_declarations[application.declaration][selected.member],XR_XIR_SOURCE_CALL) &&
        source_recipe_group(ctx,op,arguments,callable.parameter_count + 1,value);
done:
    return ok;
}
