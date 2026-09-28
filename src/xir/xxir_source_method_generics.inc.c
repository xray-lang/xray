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
static bool source_method_scope(SourceContext *ctx, SourceName *owner, uint32_t index) {
    SourceFunction *body = &ctx->bodies[index];
    MethodDeclNode *method = &body->node->as.method_decl;
    uint32_t prefix = body->type_parameter_count;
    if (method->type_param_count < 0 || (uint32_t)method->type_param_count > 65536 - prefix)
        return source_fail(ctx, body->node, XR_XIR_BUDGET, "method type parameter count exhausted");
    uint32_t own = (uint32_t)method->type_param_count, count = prefix + own;
    if (!own) return true;
    XrGenericParam **parameters = source_alloc(ctx, count, sizeof(*parameters));
    uint32_t *constraints = source_alloc(ctx, count, sizeof(*constraints));
    if (!parameters || !constraints) return false;
    for (uint32_t p = 0; p < count; ++p) {
        if (!source_work(ctx, body->node)) return false;
        if (p < prefix) {
            parameters[p] = body->type_parameters[p]; constraints[p] = ctx->generics[index].constraints[p];
            continue;
        }
        XrGenericParam *parameter = method->type_params[p - prefix];
        if (!source_parameter_markers(ctx, body->node, parameter, &constraints[p])) return false;
        for (uint32_t earlier = 0; earlier < p; ++earlier) {
            if (!source_work(ctx, body->node)) return false;
            if (!strcmp(parameter->name, parameters[earlier]->name))
                return source_fail(ctx, body->node, XR_XIR_BAD_TYPE, "method type parameter duplicates or shadows a parameter");
        }
        parameters[p] = parameter;
    }
    body->type_parameters = parameters; body->type_parameter_count = count;
    body->generic_owner = body->declaration;
    ctx->generics[index].parameter_count = count; ctx->generics[index].constraints = constraints;
    ctx->has_generics = true;
    XrXirSourceDeclaration *record = (XrXirSourceDeclaration *)&ctx->query.declarations[body->declaration - 1];
    record->generic_parent = prefix ? owner->declaration : 0;
    record->generic_parent_count = prefix; record->generic_parameter_count = count;
    return true;
}
static bool source_method_instantiation(SourceContext *ctx, AstNode *node, uint32_t index,
    SourceSubstitution enclosing, SourceTypeArguments *arguments) {
    const XrXirGeneric *generic = &ctx->generics[index];
    uint32_t owner = ctx->identities[index].nominal_owner;
    uint32_t prefix = ctx->nominals.declarations[owner - 1].parameter_count;
    if (enclosing.count != prefix || generic->parameter_count < prefix)
        return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "method enclosing parameter identity mismatch");
    XrXirGeneric own = {0}; own.parameter_count = generic->parameter_count - prefix;
    own.constraints = own.parameter_count ? generic->constraints + prefix : NULL;
    if (!source_instantiation(ctx, node, &own, arguments)) return false;
    if (!prefix) return true;
    XrXirType *types = source_alloc(ctx, generic->parameter_count, sizeof(*types));
    if (!types) return false;
    memcpy(types, enclosing.types, prefix * sizeof(*types));
    if (own.parameter_count) memcpy(types + prefix, arguments->substitution.types, own.parameter_count * sizeof(*types));
    arguments->substitution = (SourceSubstitution) {types, generic->parameter_count};
    return true;
}
