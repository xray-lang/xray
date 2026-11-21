/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_net_storage.inc.c - Opaque storage nominals of the network module
 *
 * KEY CONCEPT:
 *   Only the real standard network module receives these three names. Each one
 *   becomes a final class with no fields, so a source annotation can carry the
 *   type while the host keeps the body.
 */
#include "xxir_net_storage_native.h"
static bool source_net_storage_module(SourceContext *ctx, uint32_t module) {
    const XrModuleSpec *spec = &ctx->graph->specs[module];
    return spec->authority.kind == XR_MODULE_IDENTITY_STDLIB && spec->authority.namespace_id &&
        source_text_same(ctx, NULL, spec->authority.namespace_id, "net") && spec->logical_path &&
        source_text_same(ctx, NULL, spec->logical_path, "net/net.xr");
}
static bool source_net_storage_nominal(SourceContext *ctx, const XrNativeTypeDeclaration *native) {
    if (!source_nominal_reserve(ctx)) return false;
    uint32_t d = ctx->nominals.count;
    const char *name = source_owned_text(ctx, native->name);
    if (!name) return false;
    SourceName *owner = add_name(ctx, &ctx->names[ctx->module], name, NULL);
    if (!owner) return false;
    owner->kind = SOURCE_NOMINAL; owner->index = d; owner->module = ctx->module;
    XrXirSourceModule module = ctx->modules[ctx->module];
    XrXirNominalDeclaration record = {.module = {module.name, module.name_length},
        .name = {name, (uint32_t) strlen(name)}, .kind = XR_XIR_NOMINAL_CLASS,
        .flags = XR_XIR_NOMINAL_FINAL, .fields = NULL, .field_count = 0};
    record.native.native_id = native->id;
    if (!source_copy_bytes(ctx, NULL, record.native.source_fingerprint,
        native->source_fingerprint.bytes, 32)) return false;
    ((XrXirNominalDeclaration *) ctx->nominals.declarations)[d] = record;
    ctx->nominal_sources[d] = owner; ctx->nominal_members[d] = NULL; ++ctx->nominals.count;
    XrXirTypeNode node = {.kind = XR_XIR_TYPE_NOMINAL};
    node.nominal = (XrXirNominalType) {d, NULL, 0, NULL, 0};
    if (!source_intern_type(ctx, node, &owner->type)) return false;
    if (!source_query_declare(ctx, owner, XR_XIR_SOURCE_TYPE, 0, (XrXirSourceRange) {ctx->module, 0, 0, 0, 0}))
        return false;
    XrXirSourceDeclaration *query =
        (XrXirSourceDeclaration *) ctx->query.declarations + owner->declaration - 1;
    query->native_identity = native->id;
    query->type = (XrXirSourceType) {owner->type, 0, true};
    return true;
}
static bool source_net_storage_declare(SourceContext *ctx) {
    uint32_t saved = ctx->module;
    for (uint32_t m = 0; m < ctx->module_count; ++m) {
        if (!source_request_poll(ctx, NULL)) return false;
        if (!source_net_storage_module(ctx, m) || source_owned_checked(&ctx->graph->specs[m])) continue;
        ctx->module = m;
        for (uint32_t i = 0; i < sizeof(xir_net_storage_contracts) / sizeof(*xir_net_storage_contracts); ++i) {
            const XrNativeTypeDeclaration *native = NULL;
            if (!xir_net_storage_declaration(xir_net_storage_contracts[i].id, &native))
                return source_fail(ctx, NULL, XR_XIR_BAD_STRUCTURE, "network storage declaration is missing");
            if (find_name(ctx, ctx->names[m], native->name)) continue;
            if (!source_net_storage_nominal(ctx, native)) return false;
        }
    }
    ctx->module = saved;
    return true;
}
