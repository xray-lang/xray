/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_constructors.inc.c - Declaration-owned construction and field storage
 *
 * KEY CONCEPT:
 *   A complete nominal value is assembled only from initialized field places.
 */
typedef struct SourceConstructorScan {
    SourceContext *ctx;
    uint32_t depth;
    bool cleanup, shared;
} SourceConstructorScan;
static bool source_constructor_scan(AstNode *node, void *pointer) {
    SourceConstructorScan *scan = pointer;
    if (!node || scan->shared) return true;
    if (!source_work(scan->ctx, node)) return false;
    if (scan->depth == 128) return source_fail(scan->ctx, node, XR_XIR_BUDGET, "constructor capture depth exhausted");
    if (node->type == AST_FUNCTION_EXPR && !scan->cleanup) return true;
    if (scan->cleanup && (node->type == AST_THIS_EXPR ||
        (node->type == AST_VARIABLE && source_text_same(scan->ctx, node, node->as.variable.name, "this")))) {
        scan->shared = true; return true;
    }
    bool cleanup = scan->cleanup;
    scan->cleanup |= node->type == AST_DEFER_STMT;
    ++scan->depth;
    bool ok = xr_ast_for_each_child(node, source_constructor_scan, scan);
    --scan->depth; scan->cleanup = cleanup;
    return ok || source_fail(scan->ctx, node, XR_XIR_BAD_STRUCTURE, "unknown constructor syntax");
}
static bool source_constructor_active(SourceContext *ctx) {
    AstNode *node = ctx->bodies[ctx->function].node;
    return node && node->type == AST_METHOD_DECL && node->as.method_decl.is_constructor;
}
static bool source_constructor_receiver(SourceContext *ctx, AstNode *node) {
    if (!node || (node->type != AST_THIS_EXPR &&
        (node->type != AST_VARIABLE || !source_text_same(ctx, NULL, node->as.variable.name, "this")))) return false;
    SourceName *symbol = visible_name(ctx, "this");
    return symbol && symbol->construction;
}
static bool source_constructor_storage_type(SourceContext *ctx, XrXirType receiver, uint32_t field, XrXirType *type) {
    const XrXirTypeNode *nominal = xr_xir_type_node(&ctx->types, receiver);
    *type = nominal->nominal.fields[field];
    if (ctx->bodies[ctx->function].constructor_shared &&
        (ctx->nominals.declarations[nominal->nominal.declaration].fields[field].flags & XR_XIR_FIELD_MUTABLE))
        return source_cell_type(ctx, *type, type);
    return true;
}
static bool source_constructor_storage(SourceContext *ctx, AstNode *node, uint32_t field, SourceValue *value) {
    SourceFunction *body = &ctx->bodies[ctx->function];
    XrXirType type;
    if (!source_work(ctx, node) || !source_constructor_storage_type(ctx, visible_name(ctx, "this")->type, field, &type)) return false;
    if (body->constructor_captured) { *value = (SourceValue){body->constructor_places[field], type}; return true; }
    return source_recipe_record(ctx, (XrXirInstruction){XR_XIR_LOCAL_READ, type, {body->constructor_places[field]}, {0}, 0, {0}}, value);
}
static bool source_constructor_read(SourceContext *ctx, AstNode *node, uint32_t field, SourceValue *value) {
    if (!source_constructor_storage(ctx, node, field, value)) return false;
    if (!xr_xir_type_is_cell(&ctx->types, value->type)) return true;
    return source_recipe_record(ctx, (XrXirInstruction){XR_XIR_CELL_READ, xr_xir_cell_element(&ctx->types, value->type),
        {value->id}, {0}, 0, {0}}, value);
}
static bool source_constructor_value(SourceContext *ctx, AstNode *node, SourceValue *value) {
    XrXirType type = visible_name(ctx, "this")->type;
    const XrXirTypeNode *nominal = xr_xir_type_node(&ctx->types, type);
    uint32_t count = nominal->nominal.field_count;
    SourceValue *fields = count ? source_alloc(ctx, count, sizeof(*fields)) : NULL;
    if (count && !fields) return false;
    for (uint32_t f = 0; f < count; ++f)
        if (!source_constructor_read(ctx, node, f, &fields[f])) return false;
    return source_recipe_group(ctx, (XrXirInstruction) {xr_xir_type_is_class(&ctx->types,type) ? XR_XIR_CLASS_NEW : XR_XIR_STRUCT_NEW, type, {0}, {0}, 0, {0}}, fields, count, value);
}
static bool source_constructor_return(SourceContext *ctx, AstNode *node) {
    if (node && node->type == AST_RETURN_STMT && node->as.return_stmt.value_count)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "constructor return must be implicit or bare");
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (body->frontier) {
        if (!source_recipe_record(ctx, (XrXirInstruction){XR_XIR_CLEANUP_LEAVE, XR_XIR_UNIT, {0}, {body->block_count}, 0, {0}}, NULL)) return false;
        body->frontier = 0;
        if (!begin_block(ctx)) return false;
    }
    SourceValue value;
    if (!source_constructor_value(ctx, node, &value) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {value.id, 0}, {0}, 0, {0}}, NULL)) return false;
    ctx->returned = true; return true;
}
static bool source_constructor_store(SourceContext *ctx, AstNode *node, uint32_t field, SourceValue value) {
    SourceFunction *body = &ctx->bodies[ctx->function];
    XrXirType storage;
    if (!source_work(ctx, node) || !source_constructor_storage_type(ctx, visible_name(ctx, "this")->type, field, &storage)) return false;
    XrXirOp op = xr_xir_type_is_cell(&ctx->types, storage) ?
        (body->constructor_captured ? XR_XIR_CELL_WRITE : XR_XIR_CELL_LOCAL_WRITE) : XR_XIR_LOCAL_WRITE;
    return source_recipe_record(ctx, (XrXirInstruction) {op, XR_XIR_UNIT, {body->constructor_places[field], value.id}, {0}, 0, {0}}, NULL);
}
static bool source_constructor_field(SourceContext *ctx, AstNode *node, const char *name,
    SourceValue *value, AstNode *incoming) {
    uint32_t field; XrXirType type;
    unsigned access = incoming ? (source_constructor_active(ctx) ? 2u : 1u) : 0u;
    if (!source_struct_field(ctx, node, visible_name(ctx, "this")->type, name, access, &field, &type)) return false;
    if (!incoming) return source_constructor_read(ctx, node, field, value);
    if (!source_plan_expression(ctx, incoming, (SourceExpectedType){type != XR_XIR_UNIT,type}, value)) return false;
    if (value->type != type) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "constructor field type mismatch");
    return source_constructor_store(ctx, node, field, *value);
}
static bool source_constructor_declare(SourceContext *ctx, SourceName *owner, AstNode *node, uint32_t *next) {
    MethodDeclNode *method = &node->as.method_decl;
    if (ctx->nominal_constructors[owner->index] || *next >= ctx->first_closure || method->is_static ||
        method->is_override || method->is_static_constructor || method->is_getter || method->is_setter ||
        method->is_variadic || method->is_operator || method->attr_count || method->type_param_count || method->condition_count ||
        method->borrow_origin_count || method->borrow_origin_syntax || !method->body ||
        method->param_count < 0 || method->param_count > 65536 || (method->is_private && method->is_protected))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "constructor declaration contract is not admitted");
    uint32_t index = (*next)++; ctx->function = index; ctx->nominal_constructors[owner->index] = index;
    SourceFunction *body = &ctx->bodies[index]; body->node = node; body->module = owner->module;
    if (!source_nominal_function_scope(ctx,index,owner)) return false;
    SourceName symbol = {0}; symbol.name = "constructor"; symbol.node = node;
    if (!source_query_declare(ctx, &symbol, XR_XIR_SOURCE_FUNCTION, owner->declaration,
        source_query_range(ctx, node, "constructor"))) return false;
    body->declaration = symbol.declaration;
    uint32_t count = (uint32_t)method->param_count;
    body->parameters = count ? source_alloc(ctx, count, sizeof(*body->parameters)) : NULL;
    if (count && !body->parameters) return false;
    ctx->functions[index] = (XrXirFunction) {"constructor", 11, body->parameters, count, owner->type, NULL, 0, NULL, 0, NULL, 0};
    const XrXirNominalDeclaration *decl = &ctx->nominals.declarations[owner->index];
    for (uint32_t p = 0; p < count; ++p) {
        XrParamNode *param = method->params[p];
        if (param->passing_mode != XR_PARAM_READ || param->pattern || param->is_rest || source_text_same(ctx, NULL, param->name, "this"))
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "constructor parameter contract is not admitted");
        if (param->type) { if (!source_type(ctx, param->type, &body->parameters[p])) return false; }
        else {
            for (uint32_t f = 0; f < decl->field_count; ++f) {
                if (!source_work(ctx, node)) return false;
                if (source_text_size(ctx, param->name) == decl->fields[f].name.length &&
                    source_span_same(ctx, NULL, param->name, decl->fields[f].name.bytes, decl->fields[f].name.length)) body->parameters[p] = decl->fields[f].type;
            }
        }
        if (body->parameters[p] == XR_XIR_UNIT) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "constructor parameter needs a declaration type");
        for (uint32_t q = 0; q < p; ++q) {
            if (!source_work(ctx, node)) return false;
            if (source_text_same(ctx, NULL, param->name, method->params[q]->name)) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "duplicate constructor parameter");
        }
    }
    uint32_t access = method->is_private ? XR_XIR_MEMBER_PRIVATE : method->is_protected ? XR_XIR_MEMBER_PROTECTED : XR_XIR_MEMBER_PUBLIC;
    ctx->identities[index] = (XrXirFunctionIdentity) {owner->module, access ? 0 : decl->exported, owner->index + 1, access, 0,
        decl->kind == XR_XIR_NOMINAL_CLASS ? XR_XIR_FUNCTION_NO_SUSPEND : 0, XR_XIR_CONSTRUCTOR, 0, 0};
    return source_query_parameters(ctx, symbol.declaration);
}
static bool source_constructor_call(SourceContext *ctx, AstNode *node, XrXirType type,
    SourceName *binding, SourceName *target, SourceValue *value) {
    const XrXirTypeNode *nominal = xr_xir_type_node(&ctx->types, type);
    uint32_t index = ctx->nominal_constructors[nominal->nominal.declaration];
    if (!index) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "type has no admitted constructor");
    const XrXirFunction *function = &ctx->functions[index];
    CallExprNode *call = &node->as.call_expr;
    if (!source_argument_arity(ctx, node, index, (uint32_t)call->arg_count) ||
        (ctx->identities[index].member_access && ctx->identities[ctx->function].nominal_owner != ctx->identities[index].nominal_owner))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "constructor arity or authority mismatch");
    SourceName called = *target;
    if (ctx->bodies[index].node->type == AST_METHOD_DECL) called.declaration = ctx->bodies[index].declaration;
    if (!source_query_reference(ctx, call->callee, binding, &called, XR_XIR_SOURCE_CALL)) return false;
    SourceSubstitution substitution = {nominal->nominal.arguments, nominal->nominal.argument_count};
    SourceValue *arguments = function->parameter_count ? source_alloc(ctx, function->parameter_count, sizeof(*arguments)) : NULL;
    if (function->parameter_count && !arguments) return false;
    for (uint32_t p = 0; p < (uint32_t)call->arg_count; ++p) {
        XrXirType expected;
        if (call->arg_accesses && call->arg_accesses[p] != XR_CALL_ARG_PLAIN)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "constructor argument requires a value");
        if (!source_substitute(ctx, &substitution, function->parameters[p], 0, &expected) ||
            !source_plan_expression(ctx, call->arguments[p], (SourceExpectedType){expected != XR_XIR_UNIT,expected}, &arguments[p])) return false;
        if (arguments[p].type != expected) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "constructor argument type mismatch");
    }
    for (uint32_t p = (uint32_t)call->arg_count; p < function->parameter_count; ++p)
        if (!source_argument_default(ctx, node, index, p, &substitution, &arguments[p])) return false;
    XrXirInstruction op = {XR_XIR_CALL, type, {0}, {0}, index, {0}};
    return source_type_arguments(ctx, node, substitution.types, substitution.count, &op) &&
        source_recipe_group(ctx, op, arguments, function->parameter_count, value);
}
static bool source_constructor_body(SourceContext *ctx) {
    SourceFunction *body = &ctx->bodies[ctx->function];
    MethodDeclNode *method = &body->node->as.method_decl;
    SourceConstructorScan scan = {ctx, 0, false, false};
    if (!source_constructor_scan(method->body, &scan)) return false;
    if (scan.shared && xr_xir_type_is_class(&ctx->types,ctx->functions[ctx->function].result))
        return source_fail(ctx,body->node,XR_XIR_BAD_TYPE,"class constructor cleanup cannot capture unpublished this");
    body->constructor_shared = scan.shared;
    uint32_t owner = ctx->identities[ctx->function].nominal_owner - 1;
    const XrXirNominalDeclaration *decl = &ctx->nominals.declarations[owner];
    body->constructor_places = decl->field_count ? source_alloc(ctx, decl->field_count, sizeof(*body->constructor_places)) : NULL;
    if (decl->field_count && !body->constructor_places) return false;
    for (uint32_t p = 0; p < (uint32_t)method->param_count; ++p) {
        SourceName *symbol = add_name(ctx, &ctx->locals, method->params[p]->name, body->node);
        if (!symbol) return false;
        symbol->kind = SOURCE_LOCAL; symbol->index = p; symbol->type = body->parameters[p];
        XrParamNode *parameter = method->params[p];
        XrXirSourceRange range = {ctx->module, parameter->line, parameter->column, parameter->line, 0};
        if (parameter->column > 0 && source_text_size(ctx, symbol->name) <= (size_t)(INT_MAX - parameter->column))
            range.end_column = parameter->column + (int)source_text_size(ctx, symbol->name);
        if (!source_query_declare(ctx, symbol, XR_XIR_SOURCE_PARAMETER, body->declaration, range)) return false;
        source_query_binding_type(ctx, symbol);
    }
    SourceName *self = add_name(ctx, &ctx->locals, "this", body->node);
    if (!self) return false;
    self->kind = SOURCE_LOCAL; self->type = ctx->functions[ctx->function].result; self->construction = true;
    if (!source_query_declare(ctx, self, XR_XIR_SOURCE_BINDING, body->declaration, source_query_range(ctx, body->node, NULL))) return false;
    source_query_binding_type(ctx, self);
    for (uint32_t f = 0; f < decl->field_count; ++f) {
        SourceValue place; XrXirType storage;
        if (!source_constructor_storage_type(ctx, self->type, f, &storage)) return false;
        if (!source_work(ctx, body->node) || !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_LOCAL_UNINIT,
            storage, {0}, {0}, (decl->fields[f].flags & XR_XIR_FIELD_MUTABLE) ? 0 : 1, {0}}, &place)) return false;
        body->constructor_places[f] = place.id;
        uint32_t initializer = ctx->nominal_defaults[owner][f];
        if (initializer) {
            const XrXirTypeNode *nominal = xr_xir_type_node(&ctx->types, self->type);
            SourceValue value; XrXirInstruction op = {XR_XIR_CALL, decl->fields[f].type, {0}, {0}, initializer, {0}};
            if (!source_type_arguments(ctx, body->node, nominal->nominal.arguments, nominal->nominal.argument_count, &op) ||
                !source_recipe_record(ctx, op, &value) || !source_recipe_record(ctx, (XrXirInstruction) {xr_xir_type_is_cell(&ctx->types, storage) ? XR_XIR_CELL_LOCAL_WRITE : XR_XIR_LOCAL_WRITE, XR_XIR_UNIT, {place.id, value.id}, {0}, 0, {0}}, NULL)) return false;
        }
    }
    return statement(ctx, method->body, false) && (ctx->returned || source_constructor_return(ctx, NULL));
}
