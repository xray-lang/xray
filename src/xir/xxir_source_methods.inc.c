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
#include "xxir_source_method_generics.inc.c"
typedef struct SourceStaticMethod {
    SourceName *method;
    SourceSubstitution substitution;
} SourceStaticMethod;
static bool source_static_select(SourceContext *ctx, AstNode *node, SourceStaticMethod *selected) {
    MemberAccessNode *member = &node->as.member_access;
    AstNode *path = member->object; SourceTypeArguments arguments = {0};
    SourceName *owner = NULL, *binding = NULL;
    if (!source_nominal_path(ctx, path, &arguments, &binding, &owner)) return false;
    if (!owner || owner->kind != SOURCE_NOMINAL) return true;
    SourceName *method = find_name(ctx, ctx->nominal_methods[owner->index], member->name);
    if (!method || !method->node->as.method_decl.is_static)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "type-qualified access requires a static method");
    const XrXirFunctionIdentity *identity = &ctx->identities[method->index];
    if (identity->member_access && ctx->identities[ctx->function].nominal_owner != identity->nominal_owner)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "static method requires its declaration owner");
    if (!source_instantiation(ctx, node, (XrXirDeclarationContext){XR_XIR_CONTEXT_NOMINAL,owner->index,0}, &arguments) ||
        !source_query_reference(ctx, path, binding, owner, XR_XIR_SOURCE_TYPE_USE)) return false;
    selected->method = method; selected->substitution = arguments.substitution;
    return true;
}
static bool source_static_value(SourceContext *ctx, AstNode *node, SourceStaticMethod *selected,
    XrXirType expected, SourceValue *value) {
    const XrXirFunction *function = &ctx->functions[selected->method->index];
    uint32_t count = function->parameter_count;
    XrXirCallableParameter *parameters = count ? source_alloc(ctx, count, sizeof(*parameters)) : NULL;
    if (count && !parameters) return false;
    for (uint32_t p = 0; p < count; ++p)
        if (!source_substitute(ctx, &selected->substitution, function->parameters[p], 0, &parameters[p].type)) return false;
    XrXirType result;
    XrXirInstruction op = {XR_XIR_FUNCTION_REF, XR_XIR_UNIT, {0}, {0}, selected->method->index, {0}};
    return source_substitute(ctx, &selected->substitution, function->result, 0, &result) &&
        source_signature(ctx, parameters, count, result, &op.type) &&
        source_reference_promise(ctx, node, selected->method->index, expected, &op.type) &&
        source_type_arguments(ctx, node, selected->substitution.types, selected->substitution.count, &op) &&
        source_query_target_reference(ctx, source_query_range(ctx, node, NULL),
            selected->method->declaration, XR_XIR_SOURCE_FUNCTION_VALUE) && emit(ctx, op, value);
}
static bool source_method_call(SourceContext *ctx, AstNode *node, SourceValue receiver,
    SourceName *method, XrXirType result_context, SourceValue *value) {
    if (method->node->as.method_decl.is_static)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "static method requires type-qualified access");
    CallExprNode *call = &node->as.call_expr;
    const XrXirFunctionIdentity *identity = &ctx->identities[method->index];
    if (identity->member_access && ctx->identities[ctx->function].nominal_owner != identity->nominal_owner)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "method requires its declaration owner");
    const XrXirTypeNode *type = xr_xir_type_node(&ctx->types,receiver.type);
    SourceDirectRequest direct = {method->index,
        {type->nominal.arguments,type->nominal.argument_count},&receiver,result_context};
    SourceDirectArguments prepared = {0};
    if (!source_direct_arguments(ctx,node,&direct,&prepared)) return false;
    XrXirInstruction op = {XR_XIR_CALL,prepared.result,{0},{0},method->index,{0}};
    return source_type_arguments(ctx,node,prepared.substitution.types,prepared.substitution.count,&op) &&
        source_query_target_reference(ctx,source_query_range(ctx,call->callee,NULL),
            method->declaration,XR_XIR_SOURCE_CALL) &&
        emit_group(ctx,op,prepared.values,prepared.count,value);
}
static bool source_member_value(SourceContext *ctx, AstNode *node, SourceTypeArguments *type_arguments,
    XrXirType expected, SourceValue *value) {
    SourceEnumSelection enumeration = {0};
    if (!source_enum_select(ctx, node, &enumeration)) return false;
    if (enumeration.owner) {
        if (type_arguments->count) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "enum variant has no method type parameters");
        return source_enum_construct(ctx, node, &enumeration, NULL, value);
    }
    SourceStaticMethod selected = {0};
    if (!source_static_select(ctx, node, &selected)) return false;
    if (selected.method) {
        if (!source_method_instantiation(ctx, node, selected.method->index, selected.substitution, type_arguments)) return false;
        selected.substitution = type_arguments->substitution;
        return source_static_value(ctx, node, &selected, expected, value);
    }
    if (source_constructor_receiver(ctx, node->as.member_access.object)) {
        const XrXirNominalDeclaration *decl = &ctx->nominals.declarations[ctx->identities[ctx->function].nominal_owner - 1];
        for (uint32_t f = 0; f < decl->field_count; ++f) {
            if (!source_work(ctx, node)) return false;
            const XrXirLiteral *name = &decl->fields[f].name;
            if (strlen(node->as.member_access.name) == name->length && !memcmp(node->as.member_access.name, name->bytes, name->length)) {
                if (type_arguments->count) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "field value has no method type parameters");
                return source_constructor_field(ctx, node, node->as.member_access.name, value, NULL);
            }
        }
    }
    SourceValue receiver;
    if (!expression(ctx, node->as.member_access.object, &receiver)) return false;
    if ((uint32_t)receiver.type >= XR_XIR_TYPE_PARAMETER_BASE &&
        (uint32_t)receiver.type < XR_XIR_TYPE_PARAMETER_LIMIT) {
        SourceRequirementValueRequest request = {receiver,type_arguments,expected};
        return source_requirement_value(ctx,node,&request,value);
    }
    if (receiver.type == XR_XIR_PANIC_INFO) return source_panic_member(ctx, node, type_arguments, receiver, value);
    if (xr_xir_type_is_enum(&ctx->types, receiver.type) && !strcmp(node->as.member_access.name, "name")) {
        if (type_arguments->count) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "enum name is not generic");
        return source_enum_text(ctx, node, receiver, false, value);
    }
    if (xr_xir_type_is_enum(&ctx->types, receiver.type) && !strcmp(node->as.member_access.name, "ordinal")) {
        if (type_arguments->count) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "enum ordinal is not generic");
        uint32_t declaration = xr_xir_type_node(&ctx->types, receiver.type)->nominal.declaration;
        uint32_t member = ctx->nominal_variants[declaration][ctx->nominals.declarations[declaration].variant_count];
        return source_query_target_reference(ctx, source_query_range(ctx, node, NULL), member, XR_XIR_SOURCE_READ) &&
            emit(ctx, (XrXirInstruction) {XR_XIR_ENUM_TAG, XR_XIR_I64, {receiver.id,0}, {0}, 0, {0}}, value);
    }
    SourceName *method = source_method_find(ctx, receiver.type, node->as.member_access.name);
    if (!method) {
        if (type_arguments->count) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "field value has no method type parameters");
        return source_struct_get_value(ctx, node, receiver, value);
    }
    if (method->node->as.method_decl.is_static)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "static method value requires type-qualified access");
    const XrXirFunctionIdentity *identity = &ctx->identities[method->index];
    if (identity->member_access && ctx->identities[ctx->function].nominal_owner != identity->nominal_owner)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "method value requires its declaration owner");
    const XrXirFunction *function = &ctx->functions[method->index];
    const XrXirTypeNode *type = xr_xir_type_node(&ctx->types, receiver.type);
    if (!source_method_instantiation(ctx, node, method->index,
        (SourceSubstitution) {type->nominal.arguments, type->nominal.argument_count}, type_arguments)) return false;
    SourceSubstitution substitution = type_arguments->substitution;
    uint32_t count = function->parameter_count - 1;
    XrXirCallableParameter *parameters = count ? source_alloc(ctx, count, sizeof(*parameters)) : NULL;
    if (count && !parameters) return false;
    for (uint32_t p = 0; p < count; ++p)
        if (!source_substitute(ctx, &substitution, function->parameters[p + 1], 0, &parameters[p].type)) return false;
    XrXirType result;
    XrXirInstruction op = {XR_XIR_FUNCTION_REF, XR_XIR_UNIT, {0}, {0}, method->index, {0}};
    return source_substitute(ctx, &substitution, function->result, 0, &result) &&
        source_signature(ctx, parameters, count, result, &op.type) &&
        source_reference_promise(ctx, node, method->index, expected, &op.type) &&
        source_type_arguments(ctx, node, substitution.types, substitution.count, &op) &&
        source_query_target_reference(ctx, source_query_range(ctx, node, NULL),
            method->declaration, XR_XIR_SOURCE_FUNCTION_VALUE) &&
        emit_group(ctx, op, &receiver, 1, value);
}
/* Builtin collisions are specific to the enum access domain. */
static bool source_enum_method_name(SourceContext *ctx, SourceName *owner, AstNode *node) {
    MethodDeclNode *method = &node->as.method_decl;
    const char *name = method->name;
    if (!source_work(ctx,node) || !name) return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"enum method name is missing");
    size_t length = strlen(name);
    if (length > ctx->budget.work) return source_fail(ctx,node,XR_XIR_BUDGET,"enum method name budget exhausted");
    ctx->budget.work -= length;
    if ((!method->is_static && (!strcmp(name,"name") || !strcmp(name,"ordinal") || !strcmp(name,"toString"))) ||
        (method->is_static && !strcmp(name,"variants")))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"method conflicts with enum builtin member");
    const XrXirNominalDeclaration *declaration = &ctx->nominals.declarations[owner->index];
    for (uint32_t v = 0; v < declaration->variant_count; ++v) {
        if (!source_work(ctx,node)) return false;
        const XrXirLiteral *variant = &declaration->variants[v].name;
        if (variant->length > ctx->budget.work) return source_fail(ctx,node,XR_XIR_BUDGET,"enum variant name budget exhausted");
        ctx->budget.work -= variant->length;
        if (length == variant->length && !memcmp(name,variant->bytes,variant->length))
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"enum method conflicts with variant");
    }
    return true;
}
static bool source_nominal_methods(SourceContext *ctx, uint32_t *next) {
    for (uint32_t d = 0; d < ctx->nominals.count; ++d) {
        SourceName *owner = ctx->nominal_sources[d];
        bool enumeration = owner->node->type == AST_ENUM_DECL;
        SourceNominalDeclaration declaration;
        if (!source_nominal_declaration(ctx,owner->node,&declaration)) return false;
        ctx->module = owner->module;
        for (int m = 0; m < declaration.method_count; ++m) {
            AstNode *node = declaration.methods[m];
            if (!source_work(ctx,node)) return false;
            if (!node || node->type != AST_METHOD_DECL)
                return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"nominal method declaration is malformed");
            MethodDeclNode *method = &node->as.method_decl;
            if (method->is_constructor) {
                if (enumeration) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"enum variants provide construction");
                if (!source_constructor_declare(ctx, owner, node, next)) return false;
                continue;
            }
            if ((method->is_private && method->is_protected) ||
                method->is_override || method->is_getter || method->is_setter || method->is_static_constructor ||
                method->is_variadic || method->is_operator || method->receiver_mode != XR_PARAM_READ ||
                method->attr_count || method->borrow_origin_count ||
                method->borrow_origin_syntax || !method->body || method->param_count < 0 || method->param_count >= 65536)
                return source_fail(ctx, node, XR_XIR_BAD_TYPE, "method declaration contract is not admitted");
            if (enumeration) {
                if (!source_enum_method_name(ctx,owner,node)) return false;
            } else {
                ClassDeclNode *decl = owner->node->type == AST_CLASS_DECL ? &owner->node->as.class_decl : &owner->node->as.struct_decl;
                for (int f = 0; f < decl->field_count; ++f) {
                    if (!source_work(ctx,node)) return false;
                    if (!strcmp(method->name,decl->fields[f]->as.field_decl.name))
                        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"field and method names must be distinct");
                }
            }
            SourceName *symbol = add_name(ctx, &ctx->nominal_methods[d], method->name, node);
            if (!symbol) return false;
            if (*next >= ctx->first_closure)
                return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "method function inventory mismatch");
            uint32_t index = (*next)++; ctx->function = index;
            symbol->kind = SOURCE_FUNCTION; symbol->index = index; symbol->module = owner->module;
            SourceFunction *body = &ctx->bodies[index]; body->node = node; body->module = owner->module;
            if (!source_nominal_function_scope(ctx,index,owner)) return false;
            if (!source_query_declare(ctx, symbol, XR_XIR_SOURCE_FUNCTION, owner->declaration,
                source_query_range(ctx, node, method->name))) return false;
            body->declaration = symbol->declaration;
            if (!source_method_scope(ctx, owner, index)) return false;
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
                access ? 0 : ctx->nominals.declarations[d].exported, d + 1, access, 0, 0, method->is_static ? XR_XIR_STATIC_METHOD : XR_XIR_READ_METHOD};
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
