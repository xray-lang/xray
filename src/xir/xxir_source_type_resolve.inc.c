/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_type_resolve.inc.c - Type resolution at the actual diagnostic site
 *
 * KEY CONCEPT:
 *   A failed type lookup retains the caller's source token location.
 */
static bool source_type(SourceContext *ctx, XrTypeRef *ref, XrXirType *type) {
    bool valid = source_type_ref(ctx, ref, type);
    if (!valid && ctx->diagnostic.status != XR_XIR_OK && !ctx->diagnostic.line) {
        /* Shared primitive references carry no position; the owning declaration is the nearest one. */
        const AstNode *owner = ctx->type_scope.active ? ctx->type_scope.node : ctx->bodies[ctx->function].node;
        int line = ref && ref->line ? ref->line : owner ? owner->line : 0;
        int column = ref && ref->line ? ref->column : owner ? owner->column : 0;
        if (line) {
            ctx->diagnostic.module = ctx->module;
            ctx->diagnostic.line = line;
            ctx->diagnostic.column = column;
        }
    }
    return valid;
}
