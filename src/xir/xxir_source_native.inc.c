/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_native.inc.c - Governed native declaration binding in the source owner
 *
 * KEY CONCEPT:
 *   Generated declaration identities grant authority only after lexical lookup.
 */
static const char *source_owned_text(SourceContext *ctx, const char *text) {
    size_t length = strlen(text);
    if (length == SIZE_MAX || length + 1 > ctx->budget.work) {
        source_fail(ctx, NULL, XR_XIR_BUDGET, "source declaration text budget exhausted"); return NULL;
    }
    ctx->budget.work -= length + 1;
    char *copy = source_alloc(ctx, length + 1, 1);
    if (copy) memcpy(copy, text, length + 1);
    return copy;
}
static bool source_query_target_reference(SourceContext *ctx, XrXirSourceRange range,
    uint32_t declaration, XrXirSourceAccess access) {
    XrXirSourceReference *records = source_query_append(ctx, ctx->query.references,
        &ctx->query.reference_count, &ctx->reference_capacity, sizeof(*records));
    if (!records) return false;
    ctx->query.references = records;
    records[ctx->query.reference_count - 1] = (XrXirSourceReference) {range, declaration, declaration, access};
    return true;
}
static bool source_native_array_declaration(SourceContext *ctx) {
    if (ctx->array_declaration) return true;
    const XrNativeTypeDeclaration *native = xr_native_declaration_by_id(XR_NATIVE_DECLARATION_ARRAY);
    if (!source_work(ctx, NULL)) return false;
    if (!native || native->member_count > ctx->budget.work)
        return source_fail(ctx, NULL, XR_XIR_BUDGET, "native declaration work budget exhausted");
    ctx->budget.work -= native->member_count;
    if (!xr_native_declaration_validate(native))
        return source_fail(ctx, NULL, XR_XIR_BAD_TYPE, "native declaration authority is inconsistent");
    XrXirSourceQueryModule *modules = source_query_append(ctx, ctx->query.modules,
        &ctx->query.module_count, &ctx->query_module_capacity, sizeof(*modules));
    if (!modules) return false;
    ctx->query.modules = modules; ctx->array_module = ctx->query.module_count - 1;
    modules[ctx->array_module] = (XrXirSourceQueryModule) {
        native->identity, native->source_path, native->source_fingerprint};
    SourceName symbol = {0}; symbol.name = source_owned_text(ctx, native->name);
    if (!symbol.name) return false;
    XrXirSourceRange range = {ctx->array_module, (int) native->line, (int) native->column,
        (int) native->line, (int) (native->column + strlen(native->name))};
    if (!source_query_declare(ctx, &symbol, XR_XIR_SOURCE_TYPE, 0, range)) return false;
    ctx->array_declaration = symbol.declaration;
    XrXirSourceDeclaration *record = (XrXirSourceDeclaration *) &ctx->query.declarations[symbol.declaration - 1];
    record->native_identity = native->id; record->exported = true;
    record->generic_parameter_count = 1;
    symbol = (SourceName) {0}; symbol.name = source_owned_text(ctx, native->parameter_name);
    if (!symbol.name) return false;
    if (!source_query_declare(ctx, &symbol, XR_XIR_SOURCE_TYPE_PARAMETER, ctx->array_declaration,
        (XrXirSourceRange) {ctx->array_module, 0, 0, 0, 0})) return false;
    record = (XrXirSourceDeclaration *) &ctx->query.declarations[symbol.declaration - 1];
    record->type = (XrXirSourceType) {(XrXirType) XR_XIR_TYPE_PARAMETER_BASE, ctx->array_declaration, true};
    return true;
}
static bool source_array_element_type(SourceContext *ctx, XrXirType element, XrXirType *type) {
    XrXirModule module = {XR_XIR_BUILT, ctx->functions, ctx->function_count, NULL, ctx->generics, &ctx->types, NULL};
    XrXirStatus status = xr_xir_type_satisfies(&module, ctx->function, element, 0, &ctx->budget);
    if (status != XR_XIR_OK)
        return source_fail(ctx, NULL, status, "Array element must be a copyable storable type in this declaration");
    return source_intern_type(ctx, (XrXirTypeNode) {XR_XIR_TYPE_ARRAY, element, NULL, 0, XR_XIR_UNIT, 0, 0, {0}}, type);
}
static bool source_native_type_shadowed(SourceContext *ctx, const char *name) {
    if (find_name(ctx, ctx->locals, name) || find_name(ctx, ctx->names[ctx->module], name)) return true;
    AstNode *owner = ctx->bodies[ctx->function].type_owner;
    int count;
    XrGenericParam **parameters = source_type_parameters(ctx, &count);
    for (int i = 0; i < count; ++i) {
        if (!source_work(ctx, owner)) return true;
        if (!strcmp(name, parameters[i]->name)) return true;
    }
    /* Module declarations are hoisted, including ones whose signatures follow this one. */
    AstNode *module = ctx->graph->specs[ctx->module].ast;
    for (int i = 0; i < module->as.program.count; ++i) {
        AstNode *node = module->as.program.statements[i];
        if (!source_work(ctx, node)) return true;
        const char *declared = node->type == AST_FUNCTION_DECL ? node->as.function_decl.name :
            node->type == AST_CLASS_DECL ? node->as.class_decl.name :
            node->type == AST_STRUCT_DECL ? node->as.struct_decl.name :
            node->type == AST_VAR_DECL || node->type == AST_CONST_DECL ? node->as.var_decl.name : NULL;
        if (declared && !strcmp(declared, name)) return true;
        if (node->type == AST_IMPORT_STMT) {
            ImportStmtNode *import = &node->as.import_stmt;
            if (!import->member_count && import->alias && !strcmp(import->alias, name)) return true;
            for (int m = 0; m < import->member_count; ++m) {
                if (!source_work(ctx, node)) return true;
                const char *alias = import->members[m].alias ? import->members[m].alias : import->members[m].name;
                if (!strcmp(alias, name)) return true;
            }
        }
    }
    return false;
}
static bool source_native_array_type(SourceContext *ctx, XrTypeRef *ref, XrXirType *type) {
    const XrNativeTypeDeclaration *native = xr_native_declaration_by_name(ref->name);
    if (!native || native->id != XR_NATIVE_DECLARATION_ARRAY || ref->nchildren != native->parameter_count ||
        !ref->children || source_native_type_shadowed(ctx, ref->name))
        return source_fail(ctx, NULL, XR_XIR_BAD_TYPE, "type name does not bind the governed Array declaration");
    if (ctx->depth >= 128) return source_fail(ctx, NULL, XR_XIR_BUDGET, "source type depth exhausted");
    ++ctx->depth;
    XrXirType element;
    bool ok = source_type(ctx, ref->children[0], &element);
    --ctx->depth;
    if (!ok || !source_array_element_type(ctx, element, type) || !source_native_array_declaration(ctx)) return false;
    XrXirSourceRange range = {ctx->module, ref->line, ref->column, ref->line, 0};
    if (range.column > 0 && strlen(ref->name) <= (size_t) (INT_MAX - range.column))
        range.end_column = range.column + (int) strlen(ref->name);
    return source_query_target_reference(ctx, range, ctx->array_declaration, XR_XIR_SOURCE_TYPE_USE);
}
