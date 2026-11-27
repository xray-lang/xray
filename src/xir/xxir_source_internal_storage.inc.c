/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_internal_storage.inc.c - Opaque storage nominals of a standard module
 *
 * KEY CONCEPT:
 *   The generated table decides which storage class a standard module owns. A
 *   name resolves only while that module is being declared, and the class it
 *   becomes is final with no fields: the host keeps the body.
 */
#include "xxir_internal_storage.h"
static bool source_internal_storage_module(SourceContext *ctx, uint32_t module, const char *name) {
    const XrModuleSpec *spec = &ctx->graph->specs[module];
    return spec->authority.kind == XR_MODULE_IDENTITY_STDLIB && spec->authority.namespace_id &&
        source_text_same(ctx, NULL, spec->authority.namespace_id, name) && spec->logical_path;
}
static bool source_internal_storage_nominal(SourceContext *ctx, const XrNativeTypeDeclaration *native) {
    if (!source_nominal_reserve(ctx)) return false;
    uint32_t d = ctx->nominals.count;
    const char *name = source_owned_text(ctx, native->name);
    if (!name) return false;
    SourceName *owner = add_module_name(ctx, ctx->module, name, NULL);
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
static bool source_internal_storage_declare(SourceContext *ctx) {
    uint32_t saved = ctx->module, count = 0;
    const XrInternalStorageType *table = xr_internal_storage_table(&count);
    for (uint32_t m = 0; m < ctx->module_count; ++m) {
        if (!source_request_poll(ctx, NULL)) return false;
        if (source_owned_checked(&ctx->graph->specs[m])) continue;
        for (uint32_t i = 0; i < count; ++i) {
            if (table[i].carrier != XR_INTERNAL_STORAGE_NOMINAL) continue;
            if (!source_internal_storage_module(ctx, m, table[i].module)) continue;
            const XrNativeTypeDeclaration *native = xr_native_declaration_by_id(table[i].native_id);
            if (!native || native->kind != XR_NATIVE_DECLARATION_STORAGE || !native->name ||
                strcmp(native->name, table[i].name) || !xr_native_declaration_validate(native))
                return source_fail(ctx, NULL, XR_XIR_BAD_STRUCTURE, "internal storage declaration is missing");
            ctx->module = m;
            if (find_name(ctx, ctx->names[m], native->name)) continue;
            if (!source_internal_storage_nominal(ctx, native)) return false;
        }
    }
    ctx->module = saved;
    return true;
}
