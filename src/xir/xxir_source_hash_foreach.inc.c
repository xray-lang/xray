/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_hash_foreach.inc.c - Snapshot counting loops use ordinary indirect calls
 */
static bool source_hash_for_each(SourceContext *ctx, AstNode *node, const SourceValue *evaluated,
    XrXirType receiver_type, SourceValue *value) {
    CallExprNode *call = &node->as.call_expr; MemberAccessNode *access = &call->callee->as.member_access;
    SourceValue container = {0};
    if (evaluated) container = *evaluated;
    else if (!source_array_receiver(ctx, access->object, NULL, false, &container)) return false;
    if (container.type != receiver_type) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "hash callback receiver type changed");
    const XrXirTypeNode *type = xr_xir_type_node(&ctx->types, receiver_type);
    bool map = type->kind == XR_XIR_TYPE_MAP;
    XrXirType key = type->element, element = type->result;
    SourceArrayCallbackShape shape = {{key, element}, map ? 2u : 1u, map ? 2u : 1u,
        {true, XR_XIR_UNIT, false, false}, XR_XIR_UNIT};
    const XrNativeTypeDeclaration *native=source_hash_native(ctx,receiver_type);
    const XrNativeMemberDeclaration *member=native ? source_native_member(ctx,native,access->name) : NULL;
    if (!member || member->operation!=XR_NATIVE_OPERATION_HASH_FOR_EACH ||
        !source_native_callback_error(ctx,node,native,member,&shape.error_type)) return false;
    SourceArrayCallback callback = {0};
    if (!source_array_callback(ctx, node, call->arguments[0], &shape, &callback)) return false;
    SourceValue length = {0}, zero = {0};
    if (!source_recipe_record(ctx, (XrXirInstruction){XR_XIR_HASH_LEN, XR_XIR_I64, {container.id}, {0}, 0, {0}}, &length) ||
        !source_recipe_record(ctx, (XrXirInstruction){XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 0, {0}}, &zero)) return false;
    SourceCounter counter = {0};
    if (!source_counter_open(ctx, zero, length, false, &counter)) return false;
    SourceValue arguments[2] = {{0}};
    if (!source_recipe_record(ctx, (XrXirInstruction){XR_XIR_HASH_KEY_AT, key,
        {container.id, counter.index.id}, {0}, 0, {0}}, arguments)) return false;
    if (map && !source_recipe_record(ctx, (XrXirInstruction){XR_XIR_MAP_VALUE_AT, element,
        {container.id, counter.index.id}, {0}, 0, {0}}, arguments + 1)) return false;
    if (!source_recipe_group(ctx, (XrXirInstruction){XR_XIR_CALL_INDIRECT, XR_XIR_UNIT,
        {0}, {0}, callback.value.id, {0}}, arguments, callback.arity, NULL) || !source_counter_close(ctx, &counter)) return false;
    *value = (SourceValue){.id=UINT32_MAX, .type=XR_XIR_UNIT,.place={0}}; return true;
}
