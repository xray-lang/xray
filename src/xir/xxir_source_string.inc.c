/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_string.inc.c - Governed string value method calls
 *
 * KEY CONCEPT:
 *   The receiver snapshot survives argument effects and suspension.
 */
static bool source_string_declaration(SourceContext *ctx) {
    if (ctx->string_declaration) return true;
    const XrNativeTypeDeclaration *native = xr_native_declaration_by_id(XR_NATIVE_DECLARATION_STRING);
    if (!source_work(ctx, NULL)) return false;
    if (!native || native->member_count > ctx->budget.work)
        return source_fail(ctx, NULL, XR_XIR_BUDGET, "native string declaration work budget exhausted");
    ctx->budget.work -= native->member_count;
    if (!xr_native_declaration_validate(native))
        return source_fail(ctx, NULL, XR_XIR_BAD_TYPE, "native string declaration authority is inconsistent");
    XrXirSourceQueryModule *modules = source_query_append(ctx, ctx->query.modules,
        &ctx->query.module_count, &ctx->query_module_capacity, sizeof(*modules));
    if (!modules) return false;
    ctx->query.modules = modules; ctx->string_module = ctx->query.module_count - 1;
    modules[ctx->string_module] = (XrXirSourceQueryModule) {
        native->identity, native->source_path, native->source_fingerprint};
    SourceName symbol = {0}; symbol.name = source_owned_text(ctx, native->name);
    if (!symbol.name) return false;
    XrXirSourceRange range = {ctx->string_module, (int)native->line, (int)native->column,
        (int)native->line, (int)(native->column + strlen(native->name))};
    if (!source_query_declare(ctx, &symbol, XR_XIR_SOURCE_TYPE, 0, range)) return false;
    ctx->string_declaration = symbol.declaration;
    XrXirSourceDeclaration *record = (XrXirSourceDeclaration *)&ctx->query.declarations[symbol.declaration - 1];
    record->native_identity = native->id; record->exported = true;
    record->type = (XrXirSourceType) {XR_XIR_STRING, 0, true};
    return true;
}
static bool source_string_member_reference(SourceContext *ctx, AstNode *node,
    const XrNativeMemberDeclaration *member) {
    uint32_t index = (uint32_t)member->operation - XR_NATIVE_OPERATION_STRING_CONTAINS;
    if (index >= sizeof(ctx->string_members) / sizeof(ctx->string_members[0]))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "string member identity is not executable");
    if (!ctx->string_members[index]) {
        SourceName symbol = {0}; symbol.name = source_owned_text(ctx, member->name);
        if (!symbol.name) return false;
        XrXirSourceRange range = {ctx->string_module, (int)member->line, (int)member->column,
            (int)member->line, (int)(member->column + strlen(member->name))};
        if (!source_query_declare(ctx, &symbol, XR_XIR_SOURCE_MEMBER, ctx->string_declaration, range)) return false;
        ctx->string_members[index] = symbol.declaration;
        XrXirSourceType *parameter = source_alloc(ctx, member->parameter_count, sizeof(*parameter));
        if (!parameter) return false;
        for (uint32_t i = 0; i < member->parameter_count; ++i)
            parameter[i] = (XrXirSourceType) {i ? XR_XIR_I64 : XR_XIR_STRING, 0, true};
        XrXirSourceDeclaration *record = (XrXirSourceDeclaration *)&ctx->query.declarations[symbol.declaration - 1];
        record->native_identity = member->id;
        record->signature = source_owned_text(ctx, member->signature);
        if (!record->signature) return false;
        record->type = (XrXirSourceType) {member->result == XR_NATIVE_TERM_BOOL ? XR_XIR_BOOL : XR_XIR_I64, 0, true};
        record->parameters = parameter; record->parameter_count = member->parameter_count; record->exported = true;
    }
    return source_query_target_reference(ctx, source_query_range(ctx, node, NULL),
        ctx->string_members[index], XR_XIR_SOURCE_CALL);
}
static bool source_string_call(SourceContext *ctx, AstNode *node, SourceValue receiver, SourceValue *value) {
    CallExprNode *call = &node->as.call_expr;
    if (!source_string_declaration(ctx)) return false;
    const XrNativeTypeDeclaration *native = xr_native_declaration_by_id(XR_NATIVE_DECLARATION_STRING);
    const XrNativeMemberDeclaration *member = xr_native_declaration_member(native, call->callee->as.member_access.name);
    if (!source_work(ctx, node)) return false;
    if (!member || !member->is_public || member->is_static || !member->parameter_count ||
        member->parameters[0].type != XR_NATIVE_TERM_STRING || member->receiver != XR_NATIVE_RECEIVER_READ)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "string member has no admitted value-operation contract");
    XrXirOp operation = XR_XIR_INVALID;
    XrXirType result = XR_XIR_BOOL;
    switch (member->operation) {
    case XR_NATIVE_OPERATION_STRING_CONTAINS: operation = XR_XIR_STRING_CONTAINS; break;
    case XR_NATIVE_OPERATION_STRING_STARTS_WITH: operation = XR_XIR_STRING_STARTS_WITH; break;
    case XR_NATIVE_OPERATION_STRING_ENDS_WITH: operation = XR_XIR_STRING_ENDS_WITH; break;
    case XR_NATIVE_OPERATION_STRING_INDEX_OF: operation = XR_XIR_STRING_INDEX_OF; result = XR_XIR_I64; break;
    case XR_NATIVE_OPERATION_STRING_LAST_INDEX_OF: operation = XR_XIR_STRING_LAST_INDEX_OF; result = XR_XIR_I64; break;
    default: return source_fail(ctx, node, XR_XIR_BAD_TYPE, "string member identity is not executable");
    }
    bool indexed = operation == XR_XIR_STRING_INDEX_OF;
    if (member->parameter_count != (indexed ? 2u : 1u) ||
        member->result != (result == XR_XIR_BOOL ? XR_NATIVE_TERM_BOOL : XR_NATIVE_TERM_I64) ||
        (indexed && (member->parameters[1].type != XR_NATIVE_TERM_I64 || !member->parameters[1].optional)))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "string operation declaration shape is inconsistent");
    if (call->type_arg_count || call->default_arg_count || call->arg_count < 1 ||
        call->arg_count > (indexed ? 2 : 1) || !call->arguments)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "string operation requires its declared plain arguments");
    for (int i = 0; i < call->arg_count; ++i)
        if (call->arg_accesses && call->arg_accesses[i] != XR_CALL_ARG_PLAIN)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "string operation arguments require ordinary READ values");
    SourceValue args[3] = {receiver, {0}, {0}};
    if (!expression_in(ctx, call->arguments[0], XR_XIR_STRING, &args[1])) return false;
    if (args[1].type != XR_XIR_STRING)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "string operation pattern type mismatch");
    if (indexed) {
        if (call->arg_count == 2) {
            if (!expression_in(ctx, call->arguments[1], XR_XIR_I64, &args[2])) return false;
            if (args[2].type != XR_XIR_I64)
                return source_fail(ctx, node, XR_XIR_BAD_TYPE, "string search start requires i64");
        } else if (!emit(ctx, (XrXirInstruction) {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 0, {0}}, &args[2])) return false;
        if (!emit_group(ctx, (XrXirInstruction) {operation, result, {0}, {0}, 0, {0}}, args, 3, value)) return false;
    } else if (!emit(ctx, (XrXirInstruction) {operation, result, {args[0].id, args[1].id}, {0}, 0, {0}}, value)) return false;
    return source_string_member_reference(ctx, call->callee, member);
}
