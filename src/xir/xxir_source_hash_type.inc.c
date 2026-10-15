/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_hash_type.inc.c - Governed Map and Set type applications
 *
 * KEY CONCEPT:
 *   Ordered native binders and query provenance belong to the source owner;
 *   the complete module must prove each key's Hashable obligation.
 */
static bool source_native_hash_declaration(SourceContext *ctx,
    const XrNativeTypeDeclaration *native, uint32_t *output) {
    for (uint32_t i = 0; i < ctx->query.declaration_count; ++i) {
        if (!source_work(ctx, NULL)) return false;
        const XrXirSourceDeclaration *record = &ctx->query.declarations[i];
        if (record->kind == XR_XIR_SOURCE_TYPE && record->native_identity == native->id) {
            *output = record->id; return true;
        }
    }
    if (!source_native_admit(ctx, native)) return false;
    XrXirSourceQueryModule *modules = source_query_append(ctx, ctx->query.modules,
        &ctx->query.module_count, &ctx->query_module_capacity, sizeof(*modules));
    if (!modules) return false;
    ctx->query.modules = modules;
    uint32_t module = ctx->query.module_count - 1;
    modules[module] = (XrXirSourceQueryModule) {native->identity, native->source_path, native->source_fingerprint};
    SourceName symbol = {0}; symbol.name = source_owned_text(ctx, native->name);
    if (!symbol.name) return false;
    XrXirSourceRange range = {module, (int) native->line, (int) native->column,
        (int) native->line, (int) (native->column + source_text_size(ctx, native->name))};
    if (!source_query_declare(ctx, &symbol, XR_XIR_SOURCE_TYPE, 0, range)) return false;
    uint32_t declaration = symbol.declaration;
    XrXirConstraint *constraints = source_alloc(ctx, native->parameter_count, sizeof(*constraints));
    if (!constraints) return false;
    for (uint32_t p = 0; p < native->parameter_count; ++p)
        constraints[p] = (XrXirConstraint) {.markers = p ? 0 : XR_XIR_CONSTRAINT_HASHABLE};
    XrXirSourceDeclaration *record = (XrXirSourceDeclaration *) &ctx->query.declarations[declaration - 1];
    record->native_identity = native->id; record->exported = true;
    record->generic_parameter_count = native->parameter_count; record->generic_constraints = constraints;
    for (uint32_t p = 0; p < native->parameter_count; ++p) {
        if (!source_work(ctx, NULL)) return false;
        SourceName parameter = {0}; parameter.name = source_owned_text(ctx, native->parameter_names[p]);
        if (!parameter.name || !source_query_declare(ctx, &parameter, XR_XIR_SOURCE_TYPE_PARAMETER,
            declaration, (XrXirSourceRange) {module, 0, 0, 0, 0})) return false;
        record = (XrXirSourceDeclaration *) &ctx->query.declarations[parameter.declaration - 1];
        record->type = (XrXirSourceType) {(XrXirType) (XR_XIR_TYPE_PARAMETER_BASE + p), declaration, true};
    }
    *output = declaration; return true;
}
static bool source_native_hash_type(SourceContext *ctx, XrTypeRef *ref, XrXirType *type) {
    const XrNativeTypeDeclaration *native = source_native_find(ctx, ref->name);
    if (!native || (native->id != XR_NATIVE_DECLARATION_MAP && native->id != XR_NATIVE_DECLARATION_SET) ||
        ref->nchildren != native->parameter_count || !ref->children || source_native_type_shadowed(ctx, ref->name))
        return source_fail(ctx, NULL, XR_XIR_BAD_TYPE, "type name does not bind a governed hash container declaration");
    if (ctx->depth >= 128) return source_fail(ctx, NULL, XR_XIR_BUDGET, "source type depth exhausted");
    XrXirType arguments[2] = {XR_XIR_UNIT, XR_XIR_UNIT};
    ++ctx->depth;
    bool ok = true;
    for (uint32_t p = 0; ok && p < native->parameter_count; ++p)
        ok = source_type(ctx, ref->children[p], &arguments[p]);
    --ctx->depth;
    if (!ok) return false;
    XrXirTypeKind kind = native->id == XR_NATIVE_DECLARATION_MAP ? XR_XIR_TYPE_MAP : XR_XIR_TYPE_SET;
    uint32_t declaration = 0;
    if (!source_intern_type(ctx, (XrXirTypeNode) {.kind = kind, .element = arguments[0], .result = arguments[1]}, type) ||
        !source_native_hash_declaration(ctx, native, &declaration)) return false;
    XrXirSourceRange range = {ctx->module, ref->line, ref->column, ref->line, ref->column};
    size_t length = source_text_size(ctx, ref->name);
    if (range.column > 0 && length <= (size_t) (INT_MAX - range.column)) range.end_column += (int) length;
    return source_query_target_reference(ctx, range, declaration, XR_XIR_SOURCE_TYPE_USE);
}
