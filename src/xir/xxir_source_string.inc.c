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
#include "xxir_intrinsic.h"

static bool source_string_value_call(SourceContext *ctx,AstNode *node,SourceValue receiver,
    const XrNativeMemberDeclaration *member,SourceValue *value);
static bool source_string_declaration(SourceContext *ctx) {
    if (ctx->string_declaration) return true;
    const XrNativeTypeDeclaration *native = xr_native_declaration_by_id(XR_NATIVE_DECLARATION_STRING);
    if (!source_work(ctx, NULL)) return false;
    if (!source_native_admit(ctx,native)) return false;
    XrXirSourceQueryModule *modules = source_query_append(ctx, ctx->query.modules,
        &ctx->query.module_count, &ctx->query_module_capacity, sizeof(*modules));
    if (!modules) return false;
    ctx->query.modules = modules; ctx->string_module = ctx->query.module_count - 1;
    modules[ctx->string_module] = (XrXirSourceQueryModule) {
        native->identity, native->source_path, native->source_fingerprint};
    SourceName symbol = {0}; symbol.name = source_owned_text(ctx, native->name);
    if (!symbol.name) return false;
    XrXirSourceRange range = {ctx->string_module, (int)native->line, (int)native->column,
        (int)native->line, (int)(native->column + source_text_size(ctx, native->name))};
    if (!source_query_declare(ctx, &symbol, XR_XIR_SOURCE_TYPE, 0, range)) return false;
    ctx->string_declaration = symbol.declaration;
    XrXirSourceDeclaration *record = (XrXirSourceDeclaration *)&ctx->query.declarations[symbol.declaration - 1];
    record->native_identity = native->id; record->exported = true;
    record->type = (XrXirSourceType) {XR_XIR_STRING, 0, true};
    return true;
}
static bool source_string_member_reference(SourceContext *ctx, AstNode *node,
    const XrNativeMemberDeclaration *member) {
    const XrNativeIntrinsicDeclaration *intrinsic = xr_native_intrinsic_for_member(
        xr_native_declaration_by_id(XR_NATIVE_DECLARATION_STRING), member);
    uint32_t index = (uint32_t)member->operation - XR_NATIVE_OPERATION_STRING_CONTAINS;
    if (index >= sizeof(ctx->string_members) / sizeof(ctx->string_members[0]))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "string member identity is not executable");
    if (!ctx->string_members[index]) {
        SourceName symbol = {0}; symbol.name = source_owned_text(ctx, member->name);
        if (!symbol.name) return false;
        XrXirSourceRange range = {ctx->string_module, (int)member->line, (int)member->column,
            (int)member->line, (int)(member->column + source_text_size(ctx, member->name))};
        if (!source_query_declare(ctx, &symbol, XR_XIR_SOURCE_MEMBER, ctx->string_declaration, range)) return false;
        ctx->string_members[index] = symbol.declaration;
        XrXirSourceType *parameter = source_alloc(ctx, member->parameter_count, sizeof(*parameter));
        if (!parameter) return false;
        for (uint32_t i = 0; i < member->parameter_count; ++i) {
            XrXirType type = i ? XR_XIR_I64 : XR_XIR_STRING;
            if (intrinsic && !xr_xir_intrinsic_parameter(intrinsic, i + 1, &type))
                return source_fail(ctx, node, XR_XIR_BAD_TYPE, "intrinsic parameter is not representable");
            parameter[i] = (XrXirSourceType) {type, 0, true};
        }
        XrXirSourceDeclaration *record = (XrXirSourceDeclaration *)&ctx->query.declarations[symbol.declaration - 1];
        record->native_identity = member->id;
        record->signature = source_owned_text(ctx, member->signature);
        if (!record->signature) return false;
        XrXirType result = member->result == XR_NATIVE_TERM_BOOL ? XR_XIR_BOOL : XR_XIR_I64;
        if (intrinsic && !xr_xir_intrinsic_type(member->result, &result))
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "intrinsic result is not representable");
        record->type = (XrXirSourceType) {result, 0, true};
        record->parameters = parameter; record->parameter_count = member->parameter_count; record->exported = true;
    }
    return source_query_target_token_reference(ctx,node,
        ctx->string_members[index], XR_XIR_SOURCE_CALL);
}
static bool source_string_intrinsic_call(SourceContext *ctx, AstNode *node,
    SourceValue receiver, const XrNativeIntrinsicDeclaration *call, SourceValue *value) {
    XrXirType parameter, result;
    if (xr_xir_intrinsic(call->id) != call || call->member->parameter_count != 1 ||
        !xr_xir_intrinsic_parameter(call, 1, &parameter) ||
        !xr_xir_intrinsic_type(call->member->result, &result))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "intrinsic declaration is not executable");
    SourceValue arguments[2] = {receiver, {0}};
    uint32_t supplied = 0;
    if (!source_native_read_arguments(ctx, node, &parameter, 1, 1, &arguments[1], &supplied,
        "intrinsic requires its declared plain arguments")) return false;
    if (arguments[1].type != parameter)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "intrinsic argument type mismatch");
    XrXirInstruction instruction = {XR_XIR_CALL_INTRINSIC, result, {0}, {0}, call->id, {0}};
    return source_recipe_group(ctx, instruction, arguments, 2, value) &&
        source_string_member_reference(ctx, node->as.call_expr.callee, call->member);
}
static bool source_string_call(SourceContext *ctx, AstNode *node, SourceValue receiver, SourceValue *value) {
    CallExprNode *call = &node->as.call_expr;
    if (!source_string_declaration(ctx)) return false;
    const XrNativeTypeDeclaration *native = xr_native_declaration_by_id(XR_NATIVE_DECLARATION_STRING);
    const XrNativeMemberDeclaration *member = source_native_member(ctx,native,call->callee->as.member_access.name);
    if (!source_work(ctx, node)) return false;
    if (member && (member->operation==XR_NATIVE_OPERATION_STRING_COPY_BYTES ||
        member->operation==XR_NATIVE_OPERATION_STRING_REPLACE || member->operation==XR_NATIVE_OPERATION_STRING_REPLACE_ALL))
        return source_string_value_call(ctx,node,receiver,member,value);
    if (member && (member->operation==XR_NATIVE_OPERATION_STRING_TO_STRING ||
        member->operation==XR_NATIVE_OPERATION_STRING_REPEAT))
        return source_string_owned_call(ctx,node,receiver,member,value);
    if (!member || !member->is_public || member->is_static || !member->parameter_count ||
        member->parameters[0].type != XR_NATIVE_TERM_STRING || member->receiver != XR_NATIVE_RECEIVER_READ)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "string member has no admitted value-operation contract");
    const XrNativeIntrinsicDeclaration *intrinsic = xr_native_intrinsic_for_member(native, member);
    if (intrinsic) return source_string_intrinsic_call(ctx, node, receiver, intrinsic, value);
    XrXirOp operation = XR_XIR_INVALID;
    XrXirType result = XR_XIR_BOOL;
    switch (member->operation) {
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
    SourceValue args[3] = {receiver, {0}, {0}};
    XrXirType parameters[]={XR_XIR_STRING,XR_XIR_I64};uint32_t supplied;
    if (!source_native_read_arguments(ctx,node,parameters,1,indexed ? 2u : 1u,&args[1],&supplied,
        "string operation requires its declared plain arguments")) return false;
    if (args[1].type != XR_XIR_STRING)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "string operation pattern type mismatch");
    if (indexed) {
        if (supplied == 2) {
            if (args[2].type != XR_XIR_I64)
                return source_fail(ctx, node, XR_XIR_BAD_TYPE, "string search start requires i64");
        } else if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 0, {0}}, &args[2])) return false;
        if (!source_recipe_group(ctx, (XrXirInstruction) {operation, result, {0}, {0}, 0, {0}}, args, 3, value)) return false;
    } else if (!source_recipe_record(ctx, (XrXirInstruction) {operation, result, {args[0].id, args[1].id}, {0}, 0, {0}}, value)) return false;
    return source_string_member_reference(ctx, call->callee, member);
}
