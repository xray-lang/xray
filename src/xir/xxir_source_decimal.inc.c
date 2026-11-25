/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_decimal.inc.c - Governed decimal text conversion
 *
 * KEY CONCEPT:
 *   Lexical binding precedes declaration admission and executable selection.
 */
static XrXirType source_decimal_static_receiver(SourceContext *ctx, const MemberAccessNode *member) {
    AstNode *object = member->object;
    if (!object || object->type != AST_VARIABLE || !object->as.variable.name) return XR_XIR_UNIT;
    const char *name = object->as.variable.name;
    XrXirType scalar = XR_XIR_UNIT;
    if (name[0] == 'f' && source_text_same(ctx, object, name, "f64")) scalar = XR_XIR_F64;
    else if (name[0] == 'b' && source_text_same(ctx, object, name, "bool") &&
        source_text_same(ctx, NULL, member->name, "tryParse")) scalar = XR_XIR_BOOL;
    else if (name[0] == 'i' && source_text_same(ctx, object, name, "i64") &&
        source_text_same(ctx, NULL, member->name, "tryParse")) scalar = XR_XIR_I64;
    return scalar != XR_XIR_UNIT && source_native_type_shadowed(ctx, name) ? XR_XIR_UNIT : scalar;
}
static bool source_decimal_member_reference(SourceContext *ctx, SourceDecimalQuery *query, AstNode *node,
    const XrNativeTypeDeclaration *native, const XrNativeMemberDeclaration *member) {
    if (!query->declaration) query->declaration = source_primitive_declaration(ctx,native);
    if (!query->declaration) return false;
    if (!query->member) query->member = source_primitive_member_declaration(ctx,query->declaration,member);
    if (!query->member) return false;
    return source_query_target_token_reference(ctx, node->as.member_access.object, query->declaration, XR_XIR_SOURCE_READ) &&
        source_query_target_token_reference(ctx, node, query->member, XR_XIR_SOURCE_CALL);
}
static bool source_decimal_static_call(SourceContext *ctx, AstNode *node, XrXirType scalar, SourceValue *value) {
    CallExprNode *call = &node->as.call_expr;
    bool integer = scalar == XR_XIR_I64, boolean = scalar == XR_XIR_BOOL;
    SourceDecimalQuery *query = integer ? &ctx->i64_parse : boolean ? &ctx->bool_parse : &ctx->f64_parse;
    const XrNativeTypeDeclaration *native = xr_native_declaration_by_id(
        integer ? XR_NATIVE_DECLARATION_INTEGER : boolean ? XR_NATIVE_DECLARATION_BOOLEAN :
        XR_NATIVE_DECLARATION_FLOAT);
    if (!query->declaration && !source_native_admit(ctx,native)) return false;
    const XrNativeMemberDeclaration *member = source_native_member(ctx, native, call->callee->as.member_access.name);
    XrNativeOperation operation = integer ? XR_NATIVE_OPERATION_I64_TRY_PARSE :
        boolean ? XR_NATIVE_OPERATION_BOOL_TRY_PARSE : XR_NATIVE_OPERATION_F64_TRY_PARSE;
    XrNativeTypeTerm result = integer ? XR_NATIVE_TERM_NULLABLE_I64 :
        boolean ? XR_NATIVE_TERM_NULLABLE_BOOL : XR_NATIVE_TERM_NULLABLE_F64;
    if (!member || member->operation != operation || !member->is_public || !member->is_static ||
        !member->is_method || !member->lowered || member->receiver != XR_NATIVE_RECEIVER_READ ||
        member->parameter_count != 1 || member->parameters[0].type != XR_NATIVE_TERM_STRING ||
        member->parameters[0].optional || member->parameters[0].variadic || member->result != result ||
        member->allocation != XR_NATIVE_ALLOCATION_MAY_HEAP || member->ownership != XR_NATIVE_RESULT_OWNERSHIP_OWNED ||
        call->arg_count != 1 || !call->arguments || call->type_arg_count || call->default_arg_count ||
        (call->arg_accesses && call->arg_accesses[0] != XR_CALL_ARG_PLAIN))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, integer ?
            "integer text conversion requires its exact owned static contract" :
            boolean ? "boolean text conversion requires its exact owned static contract" :
            "binary64 text conversion requires its exact owned static contract");
    SourceValue input; XrXirType type;
    return source_plan_expression(ctx, call->arguments[0], (SourceExpectedType){true, XR_XIR_STRING, false, false}, &input) &&
        input.type == XR_XIR_STRING && source_nullable_type(ctx, scalar, &type) &&
        source_recipe_record(ctx, (XrXirInstruction){
            integer ? XR_XIR_I64_TRY_PARSE : boolean ? XR_XIR_BOOL_TRY_PARSE : XR_XIR_F64_TRY_PARSE,
            type, {input.id, 0}, {0}, 0, {0}}, value) &&
        source_decimal_member_reference(ctx, query, call->callee, native, member);
}
