/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_interfaces.inc.c - Abstract source declarations without bodies
 */
static bool source_interface_declare(SourceContext *ctx, AstNode *node) {
    if (!node || node->type != AST_INTERFACE_DECL)
        return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "interface declaration required");
    InterfaceDeclNode *declaration = &node->as.interface_decl;
    if (declaration->type_param_count < 0 || declaration->type_param_count > 65536 ||
        declaration->extends_count < 0 || declaration->method_count < 0 || declaration->property_count ||
        (declaration->type_param_count && !declaration->type_params) ||
        (declaration->extends_count && !declaration->extends) || (declaration->method_count && !declaration->methods))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "interface declaration contract is not admitted");
    for (int p = 0; p < declaration->type_param_count; ++p) {
        const XrGenericParam *parameter = declaration->type_params[p];
        if (!source_work(ctx, node)) return false;
        if (!parameter || !parameter->name || !*parameter->name)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "interface type parameter is malformed");
        for (int earlier = 0; earlier < p; ++earlier) {
            if (!source_work(ctx, node)) return false;
            if (!strcmp(parameter->name, declaration->type_params[earlier]->name))
                return source_fail(ctx, node, XR_XIR_BAD_TYPE, "duplicate interface type parameter");
        }
    }
    SourceName *symbol = add_name(ctx, &ctx->names[ctx->module], declaration->name, node);
    if (!symbol) return false;
    uint32_t count = (uint32_t)declaration->type_param_count;
    XrXirConstraint *constraints = count ? source_alloc(ctx, count, sizeof(*constraints)) : NULL;
    if (count && !constraints) return false;
    symbol->kind = SOURCE_INTERFACE; symbol->module = ctx->module; symbol->index = ctx->interfaces.count++;
    ctx->interface_sources[symbol->index] = symbol;
    const char *module = ctx->graph->specs[ctx->module].canonical;
    XrXirInterfaceDeclaration *record = (XrXirInterfaceDeclaration *)&ctx->interfaces.declarations[symbol->index];
    *record = (XrXirInterfaceDeclaration){{module,(uint32_t)strlen(module)},
        {declaration->name,(uint32_t)strlen(declaration->name)},node->is_exported,constraints,count,NULL,0,NULL,0};
    if (!source_query_declare(ctx, symbol, XR_XIR_SOURCE_TYPE, 0,
        source_query_range(ctx, node, declaration->name))) return false;
    XrXirSourceDeclaration *query = (XrXirSourceDeclaration *)&ctx->query.declarations[symbol->declaration - 1];
    query->generic_parameter_count = count; query->generic_constraints = constraints;
    return true;
}
static void source_interface_scope(SourceContext *ctx, SourceName *owner) {
    InterfaceDeclNode *declaration = &owner->node->as.interface_decl;
    ctx->module = owner->module;
    ctx->type_scope = (SourceTypeScope){true,owner->node,declaration->type_params,
        (uint32_t)declaration->type_param_count,declaration->type_param_count ? owner->declaration : 0};
}
static bool source_interface_constraints(SourceContext *ctx) {
    SourceTypeScope saved = ctx->type_scope;
    uint32_t module = ctx->module;
    bool ok = true;
    for (uint32_t d = 0; d < ctx->interfaces.count && ok; ++d) {
        SourceName *owner = ctx->interface_sources[d];
        source_interface_scope(ctx, owner);
        XrXirConstraint *constraints = (XrXirConstraint *)ctx->interfaces.declarations[d].constraints;
        for (uint32_t p = 0; p < ctx->type_scope.count && ok; ++p)
            ok = source_parameter_constraints(ctx, owner->node, ctx->type_scope.parameters[p], &constraints[p]);
    }
    ctx->type_scope = saved; ctx->module = module; return ok;
}
static bool source_interface_method_query(SourceContext *ctx, SourceName *owner,
    uint32_t member, XrXirType signature) {
    const XrXirTypeNode *callable = xr_xir_callable_signature(&ctx->types, signature);
    if (!callable) return source_fail(ctx, owner->node, XR_XIR_BAD_TYPE, "interface method signature is missing");
    uint32_t count = callable->parameter_count;
    XrXirSourceType *parameters = count ? source_alloc(ctx, count, sizeof(*parameters)) : NULL;
    if (count && !parameters) return false;
    for (uint32_t p = 0; p < count; ++p) {
        if (!source_work(ctx, owner->node)) return false;
        parameters[p] = source_query_type(ctx, callable->parameters[p].type);
    }
    uint32_t id = ctx->interface_member_declarations[owner->index][member];
    XrXirSourceDeclaration *query = (XrXirSourceDeclaration *)&ctx->query.declarations[id - 1];
    query->type = source_query_type(ctx, callable->result);
    query->parameters = parameters; query->parameter_count = count;
    query->generic_parent = ctx->type_scope.generic_owner;
    query->generic_parent_count = ctx->type_scope.count;
    query->generic_parameter_count = ctx->type_scope.count;
    query->generic_constraints = ctx->interfaces.declarations[owner->index].constraints;
    return true;
}
static bool source_interface_method(SourceContext *ctx, SourceName *owner,
    uint32_t member, XrXirInterfaceMethod *output) {
    AstNode *node = owner->node->as.interface_decl.methods[member];
    if (!node || node->type != AST_INTERFACE_METHOD)
        return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "interface method declaration required");
    InterfaceMethodNode *method = &node->as.interface_method;
    if (method->receiver_mode != XR_PARAM_READ || method->type_param_count || method->attr_count ||
        method->borrow_origin_count || method->borrow_origin_syntax || method->param_count < 0 ||
        method->param_count > 65536 || (method->param_count && !method->params))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "interface method contract is not admitted");
    SourceName *symbol = add_name(ctx, &ctx->interface_members[owner->index], method->name, node);
    if (!symbol) return false;
    symbol->kind = SOURCE_INTERFACE; symbol->index = member; symbol->module = owner->module;
    if (!source_query_declare(ctx, symbol, XR_XIR_SOURCE_MEMBER, owner->declaration,
        source_query_range(ctx, node, method->name))) return false;
    ctx->interface_member_declarations[owner->index][member] = symbol->declaration;
    uint32_t count = (uint32_t)method->param_count;
    XrXirCallableParameter *parameters = count ? source_alloc(ctx, count, sizeof(*parameters)) : NULL;
    if (count && !parameters) return false;
    for (uint32_t p = 0; p < count; ++p) {
        XrParamNode *parameter = method->params[p];
        if (!source_work(ctx, node)) return false;
        if (!parameter || !parameter->name || !*parameter->name || !strcmp(parameter->name,"this") ||
            !parameter->type || parameter->passing_mode != XR_PARAM_READ || parameter->pattern ||
            parameter->is_rest || parameter->default_value)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "interface method parameter contract is not admitted");
        for (uint32_t earlier = 0; earlier < p; ++earlier) {
            if (!source_work(ctx, node)) return false;
            if (!strcmp(parameter->name, method->params[earlier]->name))
                return source_fail(ctx, node, XR_XIR_BAD_TYPE, "duplicate interface method parameter");
        }
        if (!source_type(ctx, parameter->type, &parameters[p].type)) return false;
        if (parameters[p].type == XR_XIR_UNIT)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "interface method parameter must be a value type");
    }
    XrXirType result, signature;
    if (!source_type(ctx, method->return_type, &result) ||
        !source_signature(ctx, parameters, count, result, &signature)) return false;
    *output = (XrXirInterfaceMethod){{method->name,(uint32_t)strlen(method->name)},signature,0};
    return source_interface_method_query(ctx, owner, member, signature);
}
static bool source_interface_signature(SourceContext *ctx, SourceName *owner) {
    InterfaceDeclNode *declaration = &owner->node->as.interface_decl;
    uint32_t parents = (uint32_t)declaration->extends_count, methods = (uint32_t)declaration->method_count;
    XrXirInterfaceApplication *applications = parents ? source_alloc(ctx, parents, sizeof(*applications)) : NULL;
    XrXirInterfaceMethod *members = methods ? source_alloc(ctx, methods, sizeof(*members)) : NULL;
    uint32_t *identities = methods ? source_alloc(ctx, methods, sizeof(*identities)) : NULL;
    if ((parents && !applications) || (methods && (!members || !identities))) return false;
    ctx->interface_member_declarations[owner->index] = identities;
    for (uint32_t p = 0; p < parents; ++p)
        if (!source_work(ctx, owner->node) ||
            !source_interface_application(ctx, owner->node, declaration->extends[p], &applications[p])) return false;
    for (uint32_t m = 0; m < methods; ++m)
        if (!source_work(ctx, owner->node) || !source_interface_method(ctx, owner, m, &members[m])) return false;
    XrXirInterfaceDeclaration *record = (XrXirInterfaceDeclaration *)&ctx->interfaces.declarations[owner->index];
    record->parents = applications; record->parent_count = parents; record->methods = members; record->method_count = methods;
    return true;
}
static bool source_interface_signatures(SourceContext *ctx) {
    SourceTypeScope saved = ctx->type_scope;
    uint32_t module = ctx->module;
    bool ok = true;
    for (uint32_t d = 0; d < ctx->interfaces.count && ok; ++d) {
        SourceName *owner = ctx->interface_sources[d];
        source_interface_scope(ctx, owner);
        ok = source_interface_signature(ctx, owner);
    }
    ctx->type_scope = saved; ctx->module = module; return ok;
}
