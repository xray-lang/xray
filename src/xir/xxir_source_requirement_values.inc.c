/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_requirement_values.inc.c - Owned statically bound requirements
 *
 * KEY CONCEPT:
 *   A real generic helper preserves original requirements until specialization.
 */
typedef struct SourceRequirementValueRequest {
    SourceValue receiver;
    SourceTypeArguments *arguments;
    SourceExpectedType expected;
} SourceRequirementValueRequest;
typedef struct SourceRequirementHelper {
    SourceRequirementSelection selected;
    SourceSubstitution substitution;
    XrXirType receiver, signature;
} SourceRequirementHelper;
static bool source_requirement_helper_body(SourceContext *ctx, AstNode *site,
    const SourceRequirementHelper *request) {
    XrXirType signature;
    if (!source_requirement_prove(ctx,site,request->selected.member,
        request->selected.application,request->substitution,&signature)) return false;
    if (signature != request->signature)
        return source_fail(ctx,site,XR_XIR_BAD_STRUCTURE,"bound requirement signature changed");
    const XrXirFunction *function = &ctx->functions[ctx->function];
    uint32_t count = function->parameter_count;
    SourceValue *arguments = source_alloc(ctx,count,sizeof(*arguments));
    if (!arguments) return false;
    for (uint32_t p = 0; p < count; ++p) {
        if (!source_work(ctx,site)) return false;
        arguments[p] = (SourceValue){p,function->parameters[p]};
    }
    XrXirInstruction call = {XR_XIR_CALL_REQUIREMENT,function->result,{0},
        {request->selected.application.declaration,request->selected.member},0,{0}};
    SourceValue result;
    if (!source_type_arguments(ctx,site,request->substitution.types,
            request->substitution.count,&call) ||
        !source_recipe_group(ctx,call,arguments,count,&result) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,
            {function->result == XR_XIR_UNIT ? 0 : result.id,0},{0},0,{0}},NULL)) return false;
    ctx->returned = true;
    return finish_body(ctx);
}
static bool source_requirement_helper(SourceContext *ctx, AstNode *site,
    const SourceRequirementHelper *request, uint32_t *output) {
    if (ctx->function_count >= ctx->function_capacity || ctx->function_count >= ctx->compile.limits.functions)
        return source_fail(ctx,site,XR_XIR_BUDGET,"bound requirement function budget exhausted");
    XrXirTypeNode signature = *xr_xir_callable_signature(&ctx->types,request->signature);
    if (signature.parameter_count >= 65536)
        return source_fail(ctx,site,XR_XIR_BUDGET,"bound requirement parameter budget exhausted");
    uint32_t outer = ctx->function, index = ctx->function_count;
    SourceFunction *parent = &ctx->bodies[outer], *body = &ctx->bodies[index];
    uint32_t generic_count = ctx->generics[outer].parameter_count;
    XrXirConstraint *constraints = generic_count ? source_alloc(ctx,generic_count,sizeof(*constraints)) : NULL;
    XrXirType *parameters = source_alloc(ctx,signature.parameter_count+1,sizeof(*parameters));
    char *name = source_alloc(ctx,40,sizeof(*name));
    if (!parameters || !name || (generic_count && !constraints)) return false;
    for (uint32_t p = 0; p < generic_count; ++p) {
        if (!source_work(ctx,site)) return false;
        constraints[p] = ctx->generics[outer].constraints[p];
    }
    parameters[0] = request->receiver;
    for (uint32_t p = 0; p < signature.parameter_count; ++p) {
        if (!source_work(ctx,site)) return false;
        parameters[p+1] = signature.parameters[p].type;
    }
    if (source_format(ctx, name, 40, "$requirement%u", index) != XR_DIAG_OK)
        return source_fail(ctx, site, XR_XIR_BUDGET, "requirement name formatting exhausted");
    SourceName declaration = {0}; declaration.name = name; declaration.node = site;
    if (!source_query_declare(ctx,&declaration,XR_XIR_SOURCE_FUNCTION,parent->declaration,
        source_query_range(ctx,site,NULL))) return false;
    body->node = site; body->module = ctx->module; body->parameters = parameters;
    body->type_owner = parent->type_owner; body->generic_owner = parent->generic_owner;
    body->type_parameters = parent->type_parameters; body->type_parameter_count = parent->type_parameter_count;
    body->declaration = declaration.declaration;
    ctx->functions[index] = (XrXirFunction){name,(uint32_t)source_text_size(ctx, name),parameters,
        signature.parameter_count+1,signature.result,NULL,0,NULL,0,NULL,0};
    ctx->generics[index].parameter_count = generic_count;
    ctx->generics[index].constraints = constraints;
    ctx->identities[index] = ctx->identities[outer];
    ctx->identities[index].test_role = XR_XIR_TEST_ROLE_NONE;
    ctx->identities[index].test_timeout_seconds = 0;
    ctx->identities[index].exported = 0; ctx->identities[index].cleanup_owner = 0;
    ctx->identities[index].method_kind = ctx->identities[index].nominal_owner ? XR_XIR_MEMBER_HELPER : XR_XIR_NON_MEMBER;
    ctx->identities[index].promises = signature.flags & XR_XIR_CALLABLE_NO_SUSPEND ? XR_XIR_FUNCTION_NO_SUSPEND : 0;
    ++ctx->function_count;
    SourceName *locals = ctx->locals, *scope = ctx->scope;
    SourceLoop *loop = ctx->loop; bool returned = ctx->returned;
    SourceFact *facts = ctx->facts;SourceEpoch *epochs=ctx->epochs; uint32_t alternatives = ctx->flow_alternatives;
    ctx->function = index; ctx->locals = ctx->scope = NULL; ctx->loop = NULL; ctx->returned = false;
    ctx->facts = NULL;ctx->epochs=NULL; ctx->flow_alternatives = 0;
    bool ok = source_query_parameters(ctx,body->declaration) && source_requirement_helper_body(ctx,site,request);
    ctx->function = outer; ctx->locals = locals; ctx->scope = scope; ctx->loop = loop; ctx->returned = returned;
    ctx->facts = facts;ctx->epochs=epochs; ctx->flow_alternatives = alternatives;
    if (ok) *output = index;
    return ok;
}
static bool source_requirement_value(SourceContext *ctx, AstNode *node,
    const SourceRequirementValueRequest *request, SourceValue *value) {
    SourceRequirementHelper helper = {0}; helper.receiver = request->receiver.type;
    if (!source_requirement_select(ctx,node,request->receiver,node->as.member_access.name,&helper.selected)) return false;
    XrXirInterfaceApplication application = helper.selected.application;
    const XrXirInterfaceMethod *method = &ctx->interfaces.declarations[application.declaration].methods[helper.selected.member];
    uint32_t parent = application.argument_count, own = method->own_parameter_count;
    if (parent > 65536 || own > 65536-parent || request->arguments->count != own ||
        (own && !request->arguments->refs))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"interface method value requires its complete explicit type arguments");
    uint32_t count = parent+own;
    XrXirType *types = count ? source_alloc(ctx,count,sizeof(*types)) : NULL;
    if (count && !types) return false;
    for (uint32_t p = 0; p < parent; ++p) {
        if (!source_work(ctx,node)) return false;
        types[p] = application.arguments[p];
    }
    for (uint32_t p = 0; p < own; ++p)
        if (!source_work(ctx,node) || !source_type(ctx,request->arguments->refs[p],&types[parent+p])) return false;
    helper.substitution = (SourceSubstitution){types,count};
    if (!source_requirement_prove(ctx,node,helper.selected.member,application,helper.substitution,&helper.signature)) return false;
    XrXirTypeNode callable = *xr_xir_callable_signature(&ctx->types,helper.signature);
    const XrXirTypeNode *expected = request->expected.present ? xr_xir_callable_signature(&ctx->types,request->expected.type) : NULL;
    if (expected && expected->flags & XR_XIR_CALLABLE_NO_SUSPEND) {
        if (!(callable.flags & XR_XIR_CALLABLE_NO_SUSPEND))
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"qualified reference requires an explicit requirement promise");
    } else callable.flags &= ~XR_XIR_CALLABLE_NO_SUSPEND;
    XrXirType type;
    if (!source_intern_type(ctx,callable,&type)) return false;
    uint32_t function;
    if (!source_requirement_helper(ctx,node,&helper,&function)) return false;
    uint32_t ambient = ctx->generics[ctx->function].parameter_count;
    XrXirType *arguments = ambient ? source_alloc(ctx,ambient,sizeof(*arguments)) : NULL;
    if (ambient && !arguments) return false;
    for (uint32_t p = 0; p < ambient; ++p) {
        if (!source_work(ctx,node)) return false;
        arguments[p] = (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+p);
    }
    XrXirInstruction op = {XR_XIR_FUNCTION_REF,type,{0},{0},function,{0}};
    return source_type_arguments(ctx,node,arguments,ambient,&op) &&
        source_query_target_token_reference(ctx,node,
            ctx->interface_member_declarations[application.declaration][helper.selected.member],XR_XIR_SOURCE_FUNCTION_VALUE) &&
        source_recipe_group(ctx,op,&request->receiver,1,value);
}
