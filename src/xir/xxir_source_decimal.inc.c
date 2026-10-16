/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_decimal.inc.c - Governed binary64 text conversion
 *
 * KEY CONCEPT:
 *   Lexical binding precedes declaration admission and executable selection.
 */
static bool source_decimal_static_receiver(SourceContext *ctx, AstNode *node) {
    return node && node->type == AST_VARIABLE && source_text_same(ctx, node, node->as.variable.name, "f64") &&
        !source_native_type_shadowed(ctx, node->as.variable.name);
}
static bool source_decimal_declaration(SourceContext *ctx) {
    if (ctx->f64_declaration) return true;
    const XrNativeTypeDeclaration *native = xr_native_declaration_by_id(XR_NATIVE_DECLARATION_F64);
    if (!source_work(ctx, NULL) || !source_native_admit(ctx, native)) return false;
    XrXirSourceQueryModule *modules = source_query_append(ctx, ctx->query.modules,
        &ctx->query.module_count, &ctx->query_module_capacity, sizeof(*modules));
    if (!modules) return false;
    ctx->query.modules = modules; ctx->f64_module = ctx->query.module_count - 1;
    modules[ctx->f64_module] = (XrXirSourceQueryModule){native->identity, native->source_path, native->source_fingerprint};
    SourceName symbol = {0}; symbol.name = source_owned_text(ctx, native->name);
    if (!symbol.name) return false;
    XrXirSourceRange range = {ctx->f64_module, (int)native->line, (int)native->column,
        (int)native->line, (int)(native->column + source_text_size(ctx, native->name))};
    if (!source_query_declare(ctx, &symbol, XR_XIR_SOURCE_TYPE, 0, range)) return false;
    ctx->f64_declaration = symbol.declaration;
    XrXirSourceDeclaration *record = (XrXirSourceDeclaration *)&ctx->query.declarations[symbol.declaration - 1];
    record->native_identity = native->id; record->exported = true;
    record->type = (XrXirSourceType){XR_XIR_F64, 0, true}; return true;
}
static bool source_decimal_member_reference(SourceContext *ctx, AstNode *node,
    const XrNativeMemberDeclaration *member, XrXirType type) {
    if (!ctx->f64_parse_member) {
        SourceName symbol = {0}; symbol.name = source_owned_text(ctx, member->name);
        if (!symbol.name) return false;
        XrXirSourceRange range = {ctx->f64_module, (int)member->line, (int)member->column,
            (int)member->line, (int)(member->column + source_text_size(ctx, member->name))};
        if (!source_query_declare(ctx, &symbol, XR_XIR_SOURCE_MEMBER, ctx->f64_declaration, range)) return false;
        ctx->f64_parse_member = symbol.declaration;
        XrXirSourceType *parameter = source_alloc(ctx, 1, sizeof(*parameter));
        if (!parameter) return false;
        *parameter = (XrXirSourceType){XR_XIR_STRING, 0, true};
        XrXirSourceDeclaration *record = (XrXirSourceDeclaration *)&ctx->query.declarations[symbol.declaration - 1];
        record->native_identity = member->id; record->signature = source_owned_text(ctx, member->signature);
        if (!record->signature) return false;
        record->type = (XrXirSourceType){type, 0, true}; record->parameters = parameter;
        record->parameter_count = 1; record->exported = true;
    }
    return source_query_target_token_reference(ctx, node->as.member_access.object, ctx->f64_declaration, XR_XIR_SOURCE_READ) &&
        source_query_target_token_reference(ctx, node, ctx->f64_parse_member, XR_XIR_SOURCE_CALL);
}
static bool source_decimal_static_call(SourceContext *ctx, AstNode *node, SourceValue *value) {
    CallExprNode *call = &node->as.call_expr;
    if (!source_decimal_declaration(ctx)) return false;
    const XrNativeTypeDeclaration *native = xr_native_declaration_by_id(XR_NATIVE_DECLARATION_F64);
    const XrNativeMemberDeclaration *member = source_native_member(ctx, native, call->callee->as.member_access.name);
    if (!member || member->operation != XR_NATIVE_OPERATION_F64_TRY_PARSE || !member->is_public ||
        !member->is_static || !member->is_method || !member->lowered || member->receiver != XR_NATIVE_RECEIVER_READ ||
        member->parameter_count != 1 || member->parameters[0].type != XR_NATIVE_TERM_STRING ||
        member->parameters[0].optional || member->parameters[0].variadic || member->result != XR_NATIVE_TERM_NULLABLE_F64 ||
        member->allocation != XR_NATIVE_ALLOCATION_MAY_HEAP || member->ownership != XR_NATIVE_RESULT_OWNERSHIP_OWNED ||
        call->arg_count != 1 || !call->arguments || call->type_arg_count || call->default_arg_count ||
        (call->arg_accesses && call->arg_accesses[0] != XR_CALL_ARG_PLAIN))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "binary64 text conversion requires its exact owned static contract");
    SourceValue input; XrXirType type;
    return source_plan_expression(ctx, call->arguments[0], (SourceExpectedType){true, XR_XIR_STRING, false, false}, &input) &&
        input.type == XR_XIR_STRING && source_nullable_type(ctx, XR_XIR_F64, &type) &&
        source_recipe_record(ctx, (XrXirInstruction){XR_XIR_F64_TRY_PARSE, type, {input.id, 0}, {0}, 0, {0}}, value) &&
        source_decimal_member_reference(ctx, call->callee, member, type);
}
