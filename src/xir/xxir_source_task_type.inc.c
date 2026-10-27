/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_task_type.inc.c - Governed Task type identity and declaration use
 *
 * KEY CONCEPT:
 *   A Task descriptor grants type identity without a constructor or method authority.
 */
static bool source_native_task_declaration(SourceContext *ctx) {
    if (ctx->task_declaration) return true;
    const XrNativeTypeDeclaration *native = xr_native_declaration_by_id(XR_NATIVE_DECLARATION_TASK);
    if (!source_native_admit(ctx, native)) return false;
    if (native->parameter_count!=2 || !native->parameter_names || !native->parameter_names[0] || !native->parameter_names[1])
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"Task declaration requires value and error parameters");
    XrXirSourceQueryModule *modules = source_query_append(ctx, ctx->query.modules,
        &ctx->query.module_count, &ctx->query_module_capacity, sizeof(*modules));
    if (!modules) return false;
    ctx->query.modules = modules; ctx->task_module = ctx->query.module_count - 1;
    modules[ctx->task_module] = (XrXirSourceQueryModule) {native->identity, native->source_path, native->source_fingerprint};
    SourceName symbol = {0}; symbol.name = source_owned_text(ctx, native->name);
    if (!symbol.name || !source_query_declare(ctx, &symbol, XR_XIR_SOURCE_TYPE, 0,
        (XrXirSourceRange) {ctx->task_module, (int) native->line, (int) native->column,
            (int) native->line, (int) native->column + 4})) return false;
    ctx->task_declaration = symbol.declaration;
    XrXirConstraint *constraints = source_alloc(ctx, 2, sizeof(*constraints));
    if (!constraints) return false;
    *constraints = (XrXirConstraint) {XR_XIR_CONSTRAINT_SENDABLE, NULL, 0};
    constraints[1] = (XrXirConstraint) {XR_XIR_CONSTRAINT_ERROR | XR_XIR_CONSTRAINT_SENDABLE, NULL, 0};
    XrXirSourceDeclaration *record = (XrXirSourceDeclaration *) &ctx->query.declarations[symbol.declaration - 1];
    record->native_identity = native->id; record->exported = true;
    record->generic_parameter_count = 2; record->generic_constraints = constraints;
    for (uint32_t p=0;p<2;++p) {
        SourceName parameter = {0}; parameter.name = source_owned_text(ctx, native->parameter_names[p]);
        if (!parameter.name || !source_query_declare(ctx, &parameter, XR_XIR_SOURCE_TYPE_PARAMETER,
            ctx->task_declaration, (XrXirSourceRange) {ctx->task_module, 0, 0, 0, 0})) return false;
        record = (XrXirSourceDeclaration *) &ctx->query.declarations[parameter.declaration - 1];
        record->type = (XrXirSourceType) {(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+p), ctx->task_declaration, true};
    }
    return true;
}
static bool source_task_type(SourceContext *ctx, AstNode *node, XrXirType element, XrXirType error, XrXirType *type) {
    if (xr_xir_type_is_cell(&ctx->types, element))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Task element is not admitted in this source slice");
    if (!ctx->type_scope.active) {
        XrXirDeclarations declarations;XrXirModule module=source_module_view(ctx,&declarations);
        XrXirProofContext proof={&module,{XR_XIR_CONTEXT_FUNCTION,ctx->function,0}};
        XrXirStatus status=xr_xir_compile_type_markers_prove(&ctx->compile,&proof,error,
            XR_XIR_CONSTRAINT_ERROR | XR_XIR_CONSTRAINT_SENDABLE);
        if (status!=XR_XIR_OK) return source_fail(ctx,node,status,"Task error type must satisfy Error and Sendable");
    }
    return source_native_task_declaration(ctx) && source_intern_type(ctx,
        (XrXirTypeNode) {.kind = XR_XIR_TYPE_TASK, .element = element, .result = error}, type);
}
static bool source_native_task_type(SourceContext *ctx, XrTypeRef *ref, XrXirType *type) {
    const XrNativeTypeDeclaration *native = source_native_find(ctx, ref->name);
    if (!native || native->id != XR_NATIVE_DECLARATION_TASK || (ref->nchildren != 1 && ref->nchildren != 2) || !ref->children ||
        source_native_type_shadowed(ctx, ref->name))
        return source_fail(ctx, NULL, XR_XIR_BAD_TYPE, "type name does not bind the governed Task declaration");
    if (ctx->depth >= 128) return source_fail(ctx, NULL, XR_XIR_BUDGET, "source type depth exhausted");
    ++ctx->depth; XrXirType element,error=XR_XIR_NEVER;
    bool ok = true;
    if (ref->children[0] && ref->children[0]->kind == XR_TREF_NULL) element = XR_XIR_UNIT;
    else ok = source_type(ctx, ref->children[0], &element);
    if (ok && ref->nchildren==2) ok=source_error_type_ref(ctx,ref->children[1],&error);
    --ctx->depth;
    if (!ok || !source_task_type(ctx, NULL, element, error, type)) return false;
    XrXirSourceRange range = {ctx->module, ref->line, ref->column, ref->line, ref->column};
    return source_query_target_reference(ctx, range, ctx->task_declaration, XR_XIR_SOURCE_TYPE_USE);
}
