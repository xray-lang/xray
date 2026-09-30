/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_method_generics.inc.c - Ordered enclosing and method parameters
 *
 * KEY CONCEPT:
 *   A declaration keeps its parameter owners while XIR uses one ordered scope.
 */
static bool source_method_conditions(SourceContext *ctx, SourceFunction *body,
    XrGenericParam **parameters, XrXirConstraint *constraints, uint32_t count) {
    MethodDeclNode *method = &body->node->as.method_decl;
    if (method->condition_count < 0 || method->condition_count > 65536 ||
        (method->condition_count && !method->conditions))
        return source_fail(ctx, body->node, XR_XIR_BAD_STRUCTURE, "method conditions are malformed");
    for (int i = 0; i < method->condition_count; ++i) {
        const XrGenericParam *condition = method->conditions[i];
        uint32_t selected = UINT32_MAX;
        XrXirConstraint requirement = {0};
        if (!source_parameter_constraints(ctx,body->node,condition,&requirement)) return false;
        for (uint32_t p = 0; p < count; ++p) {
            if (!source_work(ctx, body->node)) return false;
            if (!strcmp(condition->name, parameters[p]->name)) { selected = p; break; }
        }
        if (selected == UINT32_MAX)
            return source_fail(ctx, body->node, XR_XIR_BAD_TYPE, "method condition subject is not a type parameter");
        if (!source_constraint_conjunction(ctx,body->node,constraints[selected],requirement,&constraints[selected])) return false;
    }
    return true;
}
static bool source_method_scope(SourceContext *ctx, SourceName *owner, uint32_t index) {
    SourceFunction *body = &ctx->bodies[index];
    MethodDeclNode *method = &body->node->as.method_decl;
    uint32_t prefix = body->type_parameter_count;
    if (method->type_param_count < 0 || (uint32_t)method->type_param_count > 65536 - prefix)
        return source_fail(ctx, body->node, XR_XIR_BUDGET, "method type parameter count exhausted");
    uint32_t own = (uint32_t)method->type_param_count, count = prefix + own;
    if (!own && !method->condition_count) return true;
    if (!count) return source_fail(ctx, body->node, XR_XIR_BAD_TYPE, "method condition subject is not a type parameter");
    XrGenericParam **parameters = source_alloc(ctx, count, sizeof(*parameters));
    XrXirConstraint *constraints = source_alloc(ctx, count, sizeof(*constraints));
    if (!parameters || !constraints) return false;
    for (uint32_t p = 0; p < count; ++p) {
        if (!source_work(ctx, body->node)) return false;
        if (p < prefix) {
            parameters[p] = body->type_parameters[p]; constraints[p] = ctx->generics[index].constraints[p];
            continue;
        }
        XrGenericParam *parameter = method->type_params ? method->type_params[p - prefix] : NULL;
        if (!parameter || !parameter->name || !*parameter->name)
            return source_fail(ctx,body->node,XR_XIR_BAD_TYPE,"method type parameter is malformed");
        for (uint32_t earlier = 0; earlier < p; ++earlier) {
            if (!source_work(ctx, body->node)) return false;
            if (!strcmp(parameter->name, parameters[earlier]->name))
                return source_fail(ctx, body->node, XR_XIR_BAD_TYPE, "method type parameter duplicates or shadows a parameter");
        }
        parameters[p] = parameter;
    }
    body->type_parameters = parameters; body->type_parameter_count = count;
    for (uint32_t p = prefix; p < count; ++p)
        if (!source_parameter_constraints(ctx,body->node,parameters[p],&constraints[p])) return false;
    if (!source_method_conditions(ctx, body, parameters, constraints, count)) return false;
    body->generic_owner = body->declaration;
    ctx->generics[index].parameter_count = count; ctx->generics[index].constraints = constraints;
    ctx->has_generics = true;
    XrXirSourceDeclaration *record = (XrXirSourceDeclaration *)&ctx->query.declarations[body->declaration - 1];
    record->generic_parent = prefix ? owner->declaration : 0;
    record->generic_parent_count = prefix; record->generic_parameter_count = count;
    record->generic_constraints = constraints;
    return true;
}
static bool source_method_instantiation(SourceContext *ctx, AstNode *node, uint32_t index,
    SourceSubstitution enclosing, SourceTypeArguments *arguments) {
    const XrXirGeneric *generic = &ctx->generics[index];
    uint32_t owner = ctx->identities[index].nominal_owner;
    uint32_t prefix = ctx->nominals.declarations[owner - 1].parameter_count;
    if (enclosing.count != prefix || generic->parameter_count < prefix)
        return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "method enclosing parameter identity mismatch");
    uint32_t own = generic->parameter_count - prefix;
    if (arguments->count != own || (own && !arguments->refs))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"method requires its exact explicit type arguments");
    XrXirType *types = generic->parameter_count ? source_alloc(ctx,generic->parameter_count,sizeof(*types)) : NULL;
    if (generic->parameter_count && !types) return false;
    if (prefix) memcpy(types,enclosing.types,prefix * sizeof(*types));
    for (uint32_t p = 0; p < own; ++p)
        if (!source_work(ctx,node) || !source_type(ctx,arguments->refs[p],&types[prefix + p])) return false;
    SourceSubstitution substitution = {types,generic->parameter_count};
    if (!source_instantiation_prove(ctx,node,(XrXirDeclarationContext){XR_XIR_CONTEXT_FUNCTION,index},substitution)) return false;
    arguments->substitution = substitution; return true;
}
