/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_defaults.inc.c - Proven default initialization through ordinary calls
 *
 * KEY CONCEPT:
 *   Type-graph availability is finite work; execution uses owned checked functions.
 */
static bool source_type_defaultable(SourceContext *ctx, XrXirType type) {
    if (type == XR_XIR_BOOL || xr_xir_type_is_number(type)) return true;
    const XrXirTypeNode *node = xr_xir_type_node(&ctx->types, type);
    return node && node->kind == XR_XIR_TYPE_NOMINAL && node->nominal.declaration < ctx->nominals.count &&
        ctx->nominal_defaultable[node->nominal.declaration];
}
static bool source_struct_defaultability(SourceContext *ctx) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t d = 0; d < ctx->nominals.count; ++d) {
            SourceName *symbol = ctx->nominal_sources[d];
            if (!source_work(ctx, symbol->node)) return false;
            if (ctx->nominals.declarations[d].kind != XR_XIR_NOMINAL_STRUCT || ctx->nominal_defaultable[d]) continue;
            bool explicit_constructor = false;
            ClassDeclNode *source = &symbol->node->as.struct_decl;
            for (int m = 0; m < source->method_count; ++m) {
                if (!source_work(ctx, source->methods[m])) return false;
                if (source->methods[m]->type == AST_METHOD_DECL && source->methods[m]->as.method_decl.is_constructor) explicit_constructor = true;
            }
            if (explicit_constructor) continue;
            const XrXirNominalDeclaration *decl = &ctx->nominals.declarations[d];
            bool available = true;
            for (uint32_t f = 0; f < decl->field_count; ++f) {
                AstNode *field = symbol->node->as.struct_decl.fields[f];
                if (!source_work(ctx, field)) return false;
                if (!field->as.field_decl.initializer && !source_type_defaultable(ctx, decl->fields[f].type)) {
                    available = false; break;
                }
            }
            if (!available) continue;
            if (ctx->function_count == ctx->compile.limits.functions)
                return source_fail(ctx, symbol->node, XR_XIR_BUDGET, "default constructor function budget exhausted");
            ctx->nominal_defaultable[d] = true; changed = true;
            ++ctx->function_count; ++ctx->first_closure; ++ctx->next_closure;
        }
    }
    return true;
}
static bool source_struct_constructors(SourceContext *ctx, uint32_t *next) {
    for (uint32_t d = 0; d < ctx->nominals.count; ++d) {
        SourceName *symbol = ctx->nominal_sources[d];
        if (!source_work(ctx, symbol->node)) return false;
        if (!ctx->nominal_defaultable[d]) continue;
        if (*next >= ctx->first_closure)
            return source_fail(ctx, symbol->node, XR_XIR_BAD_STRUCTURE, "default constructor function inventory mismatch");
        uint32_t index = (*next)++;
        ctx->nominal_constructors[d] = index;
        ctx->functions[index] = (XrXirFunction) {"$default", 8, NULL, 0, symbol->type, NULL, 0, NULL, 0, NULL, 0};
        ctx->bodies[index].node = symbol->node; ctx->bodies[index].module = symbol->module;
        ctx->bodies[index].declaration = symbol->declaration;
        if (!source_nominal_function_scope(ctx,index,symbol)) return false;
        ctx->identities[index] = (XrXirFunctionIdentity) {symbol->module, ctx->nominals.declarations[d].exported, d + 1, 0, 0, 0, XR_XIR_CONSTRUCTOR, 0, 0};
    }
    return true;
}
static bool source_default_value(SourceContext *ctx, AstNode *node, XrXirType type, SourceValue *value) {
    if (!source_work(ctx, node)) return false;
    if (type == XR_XIR_UNIT) {
        *value = (SourceValue){UINT32_MAX, XR_XIR_UNIT}; return true;
    }
    if (type == XR_XIR_BOOL)
        return source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CONST_BOOL, type, {0}, {0}, 0, {0}}, value);
    if (xr_xir_type_is_nullable(&ctx->types,type))
        return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_NULLABLE_NONE,type,{0},{0},0,{0}},value);
    if (xr_xir_type_is_number(type)) {
        XrXirOp op = type == XR_XIR_F32 || type == XR_XIR_F64 ? XR_XIR_CONST_FLOAT : XR_XIR_CONST_INT;
        return source_recipe_record(ctx, (XrXirInstruction) {op, type, {0}, {0}, 0, {0}}, value);
    }
    const XrXirTypeNode *found = xr_xir_type_node(&ctx->types, type);
    if (!found || found->kind != XR_XIR_TYPE_NOMINAL || !ctx->nominal_constructors[found->nominal.declaration])
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "type has no admitted default initializer");
    XrXirInstruction op = {XR_XIR_CALL, type, {0}, {0}, ctx->nominal_constructors[found->nominal.declaration], {0}};
    return source_type_arguments(ctx, node, found->nominal.arguments, found->nominal.argument_count, &op) &&
        source_recipe_record(ctx, op, value);
}
static bool source_struct_constructor_body(SourceContext *ctx) {
    uint32_t d = ctx->identities[ctx->function].nominal_owner - 1;
    const XrXirNominalDeclaration *decl = &ctx->nominals.declarations[d];
    SourceName *symbol = ctx->nominal_sources[d];
    SourceValue *fields = decl->field_count ? source_alloc(ctx, decl->field_count, sizeof(*fields)) : NULL;
    if (decl->field_count && !fields) return false;
    for (uint32_t f = 0; f < decl->field_count; ++f) {
        AstNode *field = symbol->node->as.struct_decl.fields[f];
        if (!source_work(ctx, field)) return false;
        uint32_t function = ctx->nominal_defaults[d][f];
        if (function) {
            const XrXirTypeNode *type = xr_xir_type_node(&ctx->types, symbol->type);
            XrXirInstruction op = {XR_XIR_CALL, decl->fields[f].type, {0}, {0}, function, {0}};
            if (!source_type_arguments(ctx, field, type->nominal.arguments, type->nominal.argument_count, &op) ||
                !source_recipe_record(ctx, op, &fields[f])) return false;
        } else if (!source_default_value(ctx, field, decl->fields[f].type, &fields[f])) return false;
    }
    SourceValue value;
    if (!source_recipe_group(ctx, (XrXirInstruction) {XR_XIR_STRUCT_NEW, symbol->type, {0}, {0}, 0, {0}}, fields, decl->field_count, &value) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {value.id, 0}, {0}, 0, {0}}, NULL)) return false;
    ctx->returned = true; return true;
}
