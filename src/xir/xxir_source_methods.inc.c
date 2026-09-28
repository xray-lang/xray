/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_methods.inc.c - Declaration-owned instance and static methods
 *
 * KEY CONCEPT:
 *   The receiver is one ordinary owned parameter, never a writable caller root.
 */
static SourceName *source_method_find(SourceContext *ctx, XrXirType type, const char *name) {
    const XrXirTypeNode *node = xr_xir_type_node(&ctx->types, type);
    return node && node->kind == XR_XIR_TYPE_NOMINAL ?
        find_name(ctx, ctx->nominal_methods[node->nominal.declaration], name) : NULL;
}
typedef struct SourceStaticMethod {
    SourceName *method;
    SourceSubstitution substitution;
} SourceStaticMethod;
static bool source_static_select(SourceContext *ctx, AstNode *node, SourceStaticMethod *selected) {
    MemberAccessNode *member = &node->as.member_access;
    AstNode *path = member->object;
    SourceTypeArguments arguments = {0};
    if (path->type == AST_FUNCTION_REF) {
        if (path->as.function_ref.type_arg_count < 0)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "invalid static owner type arguments");
        arguments.refs = path->as.function_ref.type_args;
        arguments.count = (uint32_t)path->as.function_ref.type_arg_count;
        path = path->as.function_ref.callee;
    }
    SourceName *owner = NULL, *binding = NULL;
    if (path->type == AST_NEW_EXPR && path->as.new_expr.is_type_namespace) {
        NewExprNode *space = &path->as.new_expr;
        if (space->module_name || space->arg_count || space->type_arg_count < 0 || !space->class_name)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "invalid static type namespace");
        binding = owner = visible_name(ctx, space->class_name);
        if (owner && owner->kind == SOURCE_IMPORT) owner = imported_declaration(ctx, owner, owner->imported);
        arguments.refs = space->type_args; arguments.count = (uint32_t)space->type_arg_count;
    } else if (path->type == AST_VARIABLE) {
        binding = owner = visible_name(ctx, path->as.variable.name);
        if (owner && owner->kind == SOURCE_IMPORT) owner = imported_declaration(ctx, owner, owner->imported);
    } else if (path->type == AST_MEMBER_ACCESS && path->as.member_access.object->type == AST_VARIABLE) {
        binding = visible_name(ctx, path->as.member_access.object->as.variable.name);
        if (binding && binding->kind == SOURCE_MODULE)
            owner = imported_declaration(ctx, binding, path->as.member_access.name);
    }
    if (ctx->diagnostic.status != XR_XIR_OK) return false;
    if (!owner || owner->kind != SOURCE_NOMINAL) return true;
    SourceName *method = find_name(ctx, ctx->nominal_methods[owner->index], member->name);
    if (!method || !method->node->as.method_decl.is_static)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "type-qualified access requires a static method");
    const XrXirFunctionIdentity *identity = &ctx->identities[method->index];
    if (identity->member_access && ctx->identities[ctx->function].nominal_owner != identity->nominal_owner)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "static method requires its declaration owner");
    if (!source_instantiation(ctx, node, &ctx->generics[method->index], &arguments) ||
        !source_query_reference(ctx, path, binding, owner, XR_XIR_SOURCE_TYPE_USE)) return false;
    selected->method = method; selected->substitution = arguments.substitution;
    return true;
}
static bool source_static_value(SourceContext *ctx, AstNode *node, SourceStaticMethod *selected, SourceValue *value) {
    const XrXirFunction *function = &ctx->functions[selected->method->index];
    uint32_t count = function->parameter_count;
    XrXirCallableParameter *parameters = count ? source_alloc(ctx, count, sizeof(*parameters)) : NULL;
    if (count && !parameters) return false;
    for (uint32_t p = 0; p < count; ++p)
        if (!source_substitute(ctx, &selected->substitution, function->parameters[p], 0, &parameters[p].type)) return false;
    XrXirType result;
    XrXirInstruction op = {XR_XIR_FUNCTION_REF, XR_XIR_UNIT, {0}, {0}, selected->method->index};
    return source_substitute(ctx, &selected->substitution, function->result, 0, &result) &&
        source_signature(ctx, parameters, count, result, &op.type) &&
        source_type_arguments(ctx, node, selected->substitution.types, selected->substitution.count, &op) &&
        source_query_target_reference(ctx, source_query_range(ctx, node, NULL),
            selected->method->declaration, XR_XIR_SOURCE_FUNCTION_VALUE) && emit(ctx, op, value);
}
static bool source_method_call(SourceContext *ctx, AstNode *node, SourceValue receiver,
    SourceName *method, SourceValue *value) {
    if (method->node->as.method_decl.is_static)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "static method requires type-qualified access");
    CallExprNode *call = &node->as.call_expr;
    const XrXirFunctionIdentity *identity = &ctx->identities[method->index];
    if (identity->member_access && ctx->identities[ctx->function].nominal_owner != identity->nominal_owner)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "method requires its declaration owner");
    const XrXirFunction *function = &ctx->functions[method->index];
    if (call->type_arg_count || !source_argument_arity(ctx, node, method->index, (uint32_t)call->arg_count + 1))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "method requires its exact value arguments");
    const XrXirTypeNode *type = xr_xir_type_node(&ctx->types, receiver.type);
    SourceSubstitution substitution = {type->nominal.arguments, type->nominal.argument_count};
    SourceValue *arguments = source_alloc(ctx, function->parameter_count, sizeof(*arguments));
    if (!arguments) return false;
    arguments[0] = receiver;
    for (uint32_t i = 1; i <= (uint32_t)call->arg_count; ++i) {
        if (call->arg_accesses && call->arg_accesses[i - 1] != XR_CALL_ARG_PLAIN)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "READ method argument cannot transfer or borrow a root");
        XrXirType expected;
        if (!source_substitute(ctx, &substitution, function->parameters[i], 0, &expected) ||
            !expression_in(ctx, call->arguments[i - 1], expected, &arguments[i])) return false;
        if (arguments[i].type != expected)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "method argument type mismatch");
    }
    for (uint32_t p = (uint32_t)call->arg_count + 1; p < function->parameter_count; ++p)
        if (!source_argument_default(ctx, node, method->index, p, &substitution, &arguments[p])) return false;
    XrXirInstruction op = {XR_XIR_CALL, XR_XIR_UNIT, {0}, {0}, method->index};
    return source_substitute(ctx, &substitution, function->result, 0, &op.type) &&
        source_type_arguments(ctx, node, substitution.types, substitution.count, &op) &&
        source_query_target_reference(ctx, source_query_range(ctx, call->callee, NULL),
            method->declaration, XR_XIR_SOURCE_CALL) &&
        emit_group(ctx, op, arguments, function->parameter_count, value);
}
static bool source_member_value(SourceContext *ctx, AstNode *node, SourceValue *value) {
    SourceStaticMethod selected = {0};
    if (!source_static_select(ctx, node, &selected)) return false;
    if (selected.method) return source_static_value(ctx, node, &selected, value);
    if (source_constructor_receiver(ctx, node->as.member_access.object)) {
        const XrXirNominalDeclaration *decl = &ctx->nominals.declarations[ctx->identities[ctx->function].nominal_owner - 1];
        for (uint32_t f = 0; f < decl->field_count; ++f) {
            if (!source_work(ctx, node)) return false;
            const XrXirLiteral *name = &decl->fields[f].name;
            if (strlen(node->as.member_access.name) == name->length && !memcmp(node->as.member_access.name, name->bytes, name->length))
                return source_constructor_field(ctx, node, node->as.member_access.name, value, NULL);
        }
    }
    SourceValue receiver;
    if (!expression(ctx, node->as.member_access.object, &receiver)) return false;
    SourceName *method = source_method_find(ctx, receiver.type, node->as.member_access.name);
    if (!method) return source_struct_get_value(ctx, node, receiver, value);
    if (method->node->as.method_decl.is_static)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "static method value requires type-qualified access");
    const XrXirFunctionIdentity *identity = &ctx->identities[method->index];
    if (identity->member_access && ctx->identities[ctx->function].nominal_owner != identity->nominal_owner)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "method value requires its declaration owner");
    const XrXirFunction *function = &ctx->functions[method->index];
    const XrXirTypeNode *type = xr_xir_type_node(&ctx->types, receiver.type);
    SourceSubstitution substitution = {type->nominal.arguments, type->nominal.argument_count};
    uint32_t count = function->parameter_count - 1;
    XrXirCallableParameter *parameters = count ? source_alloc(ctx, count, sizeof(*parameters)) : NULL;
    if (count && !parameters) return false;
    for (uint32_t p = 0; p < count; ++p)
        if (!source_substitute(ctx, &substitution, function->parameters[p + 1], 0, &parameters[p].type)) return false;
    XrXirType result;
    XrXirInstruction op = {XR_XIR_FUNCTION_REF, XR_XIR_UNIT, {0}, {0}, method->index};
    return source_substitute(ctx, &substitution, function->result, 0, &result) &&
        source_signature(ctx, parameters, count, result, &op.type) &&
        source_type_arguments(ctx, node, substitution.types, substitution.count, &op) &&
        source_query_target_reference(ctx, source_query_range(ctx, node, NULL),
            method->declaration, XR_XIR_SOURCE_FUNCTION_VALUE) &&
        emit_group(ctx, op, &receiver, 1, value);
}
static bool source_struct_methods(SourceContext *ctx, uint32_t *next) {
    for (uint32_t d = 0; d < ctx->nominals.count; ++d) {
        SourceName *owner = ctx->nominal_sources[d];
        ClassDeclNode *decl = &owner->node->as.struct_decl;
        ctx->module = owner->module;
        for (int m = 0; m < decl->method_count; ++m) {
            AstNode *node = decl->methods[m];
            if (!source_work(ctx, node) || node->type != AST_METHOD_DECL) return false;
            MethodDeclNode *method = &node->as.method_decl;
            if (method->is_constructor) {
                if (!source_constructor_declare(ctx, owner, node, next)) return false;
                continue;
            }
            if ((method->is_private && method->is_protected) ||
                method->is_override || method->is_getter || method->is_setter || method->is_static_constructor ||
                method->is_variadic || method->is_operator || method->receiver_mode != XR_PARAM_READ ||
                method->attr_count || method->type_param_count || method->borrow_origin_count ||
                method->borrow_origin_syntax || !method->body || method->param_count < 0 || method->param_count >= 65536)
                return source_fail(ctx, node, XR_XIR_BAD_TYPE, "method declaration contract is not admitted");
            for (int f = 0; f < decl->field_count; ++f) {
                if (!source_work(ctx, node)) return false;
                if (!strcmp(method->name, decl->fields[f]->as.field_decl.name))
                    return source_fail(ctx, node, XR_XIR_BAD_TYPE, "field and method names must be distinct");
            }
            SourceName *symbol = add_name(ctx, &ctx->nominal_methods[d], method->name, node);
            if (!symbol) return false;
            if (*next >= ctx->first_closure)
                return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "method function inventory mismatch");
            uint32_t index = (*next)++; ctx->function = index;
            symbol->kind = SOURCE_FUNCTION; symbol->index = index; symbol->module = owner->module;
            SourceFunction *body = &ctx->bodies[index]; body->node = node; body->module = owner->module;
            source_struct_function_scope(ctx, index, owner);
            if (!source_query_declare(ctx, symbol, XR_XIR_SOURCE_FUNCTION, owner->declaration,
                source_query_range(ctx, node, method->name))) return false;
            body->declaration = symbol->declaration;
            uint32_t offset = method->is_static ? 0 : 1;
            uint32_t count = (uint32_t)method->param_count + offset;
            body->parameters = count ? source_alloc(ctx, count, sizeof(*body->parameters)) : NULL;
            if (count && !body->parameters) return false;
            if (offset) body->parameters[0] = owner->type;
            XrXirFunction *function = &ctx->functions[index];
            *function = (XrXirFunction) {method->name, (uint32_t)strlen(method->name), body->parameters,
                count, XR_XIR_UNIT, NULL, 0, NULL, 0, NULL, 0};
            if (!source_type(ctx, method->return_type, &function->result)) return false;
            for (int i = 0; i < method->param_count; ++i) {
                XrParamNode *param = method->params[i];
                if (!param->type || param->passing_mode != XR_PARAM_READ ||
                    param->pattern || param->is_rest || !strcmp(param->name, "this") ||
                    !source_type(ctx, param->type, &body->parameters[i + offset]) || body->parameters[i + offset] == XR_XIR_UNIT)
                    return source_fail(ctx, node, XR_XIR_BAD_TYPE, "method parameter contract is not admitted");
                for (int j = 0; j < i; ++j) {
                    if (!source_work(ctx, node)) return false;
                    if (!strcmp(param->name, method->params[j]->name))
                        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "duplicate method parameter");
                }
            }
            uint32_t access = method->is_private ? XR_XIR_MEMBER_PRIVATE :
                method->is_protected ? XR_XIR_MEMBER_PROTECTED : XR_XIR_MEMBER_PUBLIC;
            ctx->identities[index] = (XrXirFunctionIdentity) {owner->module,
                access ? 0 : ctx->nominals.declarations[d].exported, d + 1, access};
            if (!source_query_parameters(ctx, symbol->declaration)) return false;
        }
    }
    return true;
}
static bool source_method_body(SourceContext *ctx) {
    SourceFunction *body = &ctx->bodies[ctx->function];
    MethodDeclNode *method = &body->node->as.method_decl;
    uint32_t offset = method->is_static ? 0 : 1;
    for (uint32_t i = 0; i < (uint32_t)method->param_count + offset; ++i) {
        const char *name = i >= offset ? method->params[i - offset]->name : "this";
        SourceName *symbol = add_name(ctx, &ctx->locals, name, body->node);
        if (!symbol) return false;
        symbol->kind = SOURCE_LOCAL; symbol->index = i; symbol->type = body->parameters[i];
        XrXirSourceRange range = source_query_range(ctx, body->node, NULL);
        if (i >= offset) {
            const XrParamNode *parameter = method->params[i - offset];
            range = (XrXirSourceRange) {ctx->module, parameter->line, parameter->column, parameter->line, 0};
            if (parameter->column > 0 && strlen(name) <= (size_t)(INT_MAX - parameter->column))
                range.end_column = parameter->column + (int)strlen(name);
        }
        if (!source_query_declare(ctx, symbol, XR_XIR_SOURCE_PARAMETER, body->declaration, range)) return false;
        source_query_binding_type(ctx, symbol);
    }
    return statement(ctx, method->body, false);
}
