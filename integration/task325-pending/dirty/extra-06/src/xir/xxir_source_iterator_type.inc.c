/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_iterator_type.inc.c - Governed Iterator type identity and declaration use
 *
 * KEY CONCEPT:
 *   A governed signature grants only its corresponding canonical pull operation.
 */
static bool source_native_iterator_declaration(SourceContext *ctx) {
    if (ctx->iterator_declaration) return true;
    const XrNativeTypeDeclaration *native = xr_native_declaration_by_id(XR_NATIVE_DECLARATION_ITERATOR);
    if (!source_native_admit(ctx, native)) return false;
    XrXirSourceQueryModule *modules = source_query_append(ctx, ctx->query.modules,
        &ctx->query.module_count, &ctx->query_module_capacity, sizeof(*modules));
    if (!modules) return false;
    ctx->query.modules = modules; ctx->iterator_module = ctx->query.module_count - 1;
    modules[ctx->iterator_module] = (XrXirSourceQueryModule) {native->identity, native->source_path, native->source_fingerprint};
    SourceName symbol = {0}; symbol.name = source_owned_text(ctx, native->name);
    if (!symbol.name || !source_query_declare(ctx, &symbol, XR_XIR_SOURCE_TYPE, 0,
        (XrXirSourceRange) {ctx->iterator_module, (int) native->line, (int) native->column,
            (int) native->line, (int) native->column + 8})) return false;
    ctx->iterator_declaration = symbol.declaration;
    XrXirConstraint *constraints = source_alloc(ctx, 1, sizeof(*constraints));
    if (!constraints) return false;
    *constraints = (XrXirConstraint) {0};
    XrXirSourceDeclaration *record = (XrXirSourceDeclaration *) &ctx->query.declarations[symbol.declaration - 1];
    record->native_identity = native->id; record->exported = true;
    record->generic_parameter_count = 1; record->generic_constraints = constraints;
    SourceName parameter = {0}; parameter.name = source_owned_text(ctx, native->parameter_names[0]);
    if (!parameter.name || !source_query_declare(ctx, &parameter, XR_XIR_SOURCE_TYPE_PARAMETER,
        ctx->iterator_declaration, (XrXirSourceRange) {ctx->iterator_module, 0, 0, 0, 0})) return false;
    record = (XrXirSourceDeclaration *) &ctx->query.declarations[parameter.declaration - 1];
    record->type = (XrXirSourceType) {(XrXirType) XR_XIR_TYPE_PARAMETER_BASE, ctx->iterator_declaration, true};
    return true;
}
static bool source_iterator_type(SourceContext *ctx, XrXirType element, XrXirType *type) {
    if (xr_xir_type_is_cell(&ctx->types, element))
        return source_fail(ctx, NULL, XR_XIR_BAD_TYPE, "Iterator element cannot be a place");
    return source_native_iterator_declaration(ctx) && source_intern_type(ctx,
        (XrXirTypeNode) {.kind = XR_XIR_TYPE_ITERATOR, .element = element}, type);
}
static bool source_native_iterator_type(SourceContext *ctx, XrTypeRef *ref, XrXirType *type) {
    const XrNativeTypeDeclaration *native = source_native_find(ctx, ref->name);
    if (!native || native->id != XR_NATIVE_DECLARATION_ITERATOR || ref->nchildren != 1 || !ref->children ||
        source_native_type_shadowed(ctx, ref->name))
        return source_fail(ctx, NULL, XR_XIR_BAD_TYPE, "type name does not bind the governed Iterator declaration");
    if (ctx->depth >= 128) return source_fail(ctx, NULL, XR_XIR_BUDGET, "source type depth exhausted");
    ++ctx->depth; XrXirType element;
    bool ok = true;
    if (ref->children[0] && ref->children[0]->kind == XR_TREF_NULL) element = XR_XIR_UNIT;
    else ok = source_type(ctx, ref->children[0], &element);
    --ctx->depth;
    if (!ok || !source_iterator_type(ctx, element, type)) return false;
    XrXirSourceRange range = {ctx->module, ref->line, ref->column, ref->line, ref->column};
    return source_query_target_reference(ctx, range, ctx->iterator_declaration, XR_XIR_SOURCE_TYPE_USE);
}
static const XrNativeMemberDeclaration *source_iterator_member(SourceContext *ctx,AstNode *node,const char *name) {
    if(!source_native_iterator_declaration(ctx)) return NULL;
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_ITERATOR);
    const XrNativeMemberDeclaration *member=source_native_member(ctx,native,name);
    if(!source_work(ctx,node)) return NULL;
    if(!member || !member->is_public || !member->is_method || member->is_static || !member->lowered ||
        member->parameter_count!=(member->operation==XR_NATIVE_OPERATION_ITERATOR_NTH ? 1u : 0u) ||
        (member->operation!=XR_NATIVE_OPERATION_ITERATOR_HAS_NEXT && member->operation!=XR_NATIVE_OPERATION_ITERATOR_NEXT &&
        member->operation!=XR_NATIVE_OPERATION_ITERATOR_NTH)) {
        source_fail(ctx,node,XR_XIR_BAD_TYPE,"Iterator member has no admitted pull-operation contract");return NULL;
    }
    return member;
}
static bool source_iterator_member_reference(SourceContext *ctx,AstNode *node,const XrNativeMemberDeclaration *member) {
    uint32_t index=member->operation==XR_NATIVE_OPERATION_ITERATOR_NTH ? 2u :
        member->operation==XR_NATIVE_OPERATION_ITERATOR_NEXT;
    if(!ctx->iterator_members[index]) {
        SourceName symbol={0};symbol.name=source_owned_text(ctx,member->name);if(!symbol.name) return false;
        XrXirSourceRange range={ctx->iterator_module,(int)member->line,(int)member->column,
            (int)member->line,(int)(member->column+source_text_size(ctx,member->name))};
        if(!source_query_declare(ctx,&symbol,XR_XIR_SOURCE_MEMBER,ctx->iterator_declaration,range)) return false;
        ctx->iterator_members[index]=symbol.declaration;
        char *signature=(char *)source_owned_text(ctx,member->signature);if(!signature) return false;
        XrXirSourceDeclaration *record=(XrXirSourceDeclaration *)&ctx->query.declarations[symbol.declaration-1];
        record->native_identity=member->id;record->signature=signature;record->exported=true;
        record->generic_parent=ctx->iterator_declaration;record->generic_parent_count=1;
        record->type=index ? (XrXirSourceType){(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,ctx->iterator_declaration,true} :
            (XrXirSourceType){XR_XIR_BOOL,0,true};
    }
    return source_query_target_token_reference(ctx,node,ctx->iterator_members[index],XR_XIR_SOURCE_CALL);
}
