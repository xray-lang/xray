/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_runtime.inc.c - Governed counter declarations in the standard runtime module
 *
 * KEY CONCEPT:
 *   Only the real standard runtime module receives the five execution-domain
 *   counter leaves. Each leaf is an exact zero-argument read returning one
 *   integer, so the source layer never invents a shape for a host object.
 */
#include "xxir_runtime_native.h"
static bool source_runtime_module(SourceContext *ctx, uint32_t module) {
    const XrModuleSpec *spec = &ctx->graph->specs[module];
    return spec->authority.kind == XR_MODULE_IDENTITY_STDLIB && spec->authority.namespace_id &&
        source_text_same(ctx, NULL, spec->authority.namespace_id, "runtime") && spec->logical_path &&
        source_text_same(ctx, NULL, spec->logical_path, "runtime/runtime.xr");
}
static bool source_runtime_declare(SourceContext *ctx) {
    uint32_t saved_module = ctx->module, saved_function = ctx->function;
    for (uint32_t m = 0; m < ctx->module_count; ++m) {
        if (!source_request_poll(ctx, NULL)) return false;
        if (!source_runtime_module(ctx, m) || source_owned_checked(&ctx->graph->specs[m])) continue;
        ctx->module = m;
        for (uint32_t item = 0; item < XIR_RUNTIME_NATIVE_COUNT; ++item) {
            uint32_t id = xir_runtime_native_ids[item];
            const XrNativeCallableDeclaration *native = xr_native_callable_by_id(id);
            if (!native || !native->member || !native->member->name)
                return source_fail(ctx, NULL, XR_XIR_BAD_STRUCTURE, "runtime counter declaration is missing");
            if (find_name(ctx, ctx->names[m], native->member->name)) continue;
            if (ctx->function_count >= ctx->function_capacity)
                return source_fail(ctx, NULL, XR_XIR_BUDGET, "runtime native callable inventory exhausted");
            if (!xir_runtime_native_signature(id, &ctx->types, NULL, 0, XR_XIR_I64, XR_XIR_NEVER))
                return source_fail(ctx, NULL, XR_XIR_BAD_TYPE, "runtime counter differs from its governed signature");
            const char *name = source_owned_text(ctx, native->member->name);
            const char *signature = source_owned_text(ctx, native->member->signature);
            if (!name || !signature) return false;
            SourceName *symbol = add_name(ctx, &ctx->names[m], name, NULL);
            if (!symbol) return false;
            uint32_t index = ctx->function_count++;
            symbol->kind = SOURCE_FUNCTION; symbol->index = index; symbol->module = m;
            ctx->functions[index] = (XrXirFunction) {name, (uint32_t) strlen(name), NULL, 0,
                XR_XIR_I64, NULL, 0, NULL, 0, NULL, 0, XR_XIR_NEVER, 0, NULL, 0, NULL};
            ctx->identities[index] = (XrXirFunctionIdentity) {m,
                .promises = XR_XIR_FUNCTION_NO_SUSPEND, .native_callable = id};
            ctx->bodies[index].module = m; ctx->bodies[index].parameters = NULL;
            if (!source_query_declare(ctx, symbol, XR_XIR_SOURCE_FUNCTION, 0, (XrXirSourceRange) {m, 0, 0, 0, 0}))
                return false;
            ((XrXirSourceDeclaration *) ctx->query.declarations)[symbol->declaration - 1].signature = signature;
            ctx->bodies[index].declaration = symbol->declaration; ctx->function = index;
            if (!source_query_parameters(ctx, symbol->declaration)) return false;
        }
    }
    ctx->module = saved_module; ctx->function = saved_function; return true;
}
