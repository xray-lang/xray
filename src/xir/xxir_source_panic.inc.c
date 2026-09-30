/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_panic.inc.c - Read-only PanicInfo members
 *
 * KEY CONCEPT:
 *   A caught PanicInfo exposes its message and runtime error code. Members
 *   that need class identity, optional references or JSON values are
 *   rejected by name rather than approximated.
 */
static bool source_panic_member(SourceContext *ctx, AstNode *node, const SourceTypeArguments *type_arguments,
    SourceValue receiver, SourceValue *value) {
    const char *name = node->as.member_access.name;
    if (type_arguments->count) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "PanicInfo members have no type parameters");
    if (name && !strcmp(name, "code"))
        return source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_PANIC_CODE, XR_XIR_I64, {receiver.id, 0}, {0}, 0, {0}}, value);
    if (name && !strcmp(name, "message"))
        return source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_PANIC_MESSAGE, XR_XIR_STRING, {receiver.id, 0}, {0}, 0, {0}}, value);
    return source_fail(ctx, node, XR_XIR_BAD_TYPE, "PanicInfo exposes only message and code without class support");
}
