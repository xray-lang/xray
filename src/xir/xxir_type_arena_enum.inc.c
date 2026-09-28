/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_type_arena_enum.inc.c - Presealed empty enum descriptors with weak backpointers
 */
static XirNominalValue ***arena_empty_variants(ArenaNominalCursor *c,
    const XrXirTypes *types, XrXirTypeArena *arena) {
    if (!types || !types->nominals || c->status != XR_XIR_VALUE_OK) return NULL;
    bool found = false;
    for (uint32_t i = 0; i < types->count; ++i) {
        if (!c->work) { c->status = XR_XIR_VALUE_LIMIT; return NULL; }
        --c->work;
        if (xr_xir_type_is_enum(types, (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + i))) found = true;
    }
    if (!found) return NULL;
    XirNominalValue ***map = arena_nominal_span(c, types->count, sizeof(*map), _Alignof(XirNominalValue **));
    for (uint32_t i = 0; i < types->count && c->status == XR_XIR_VALUE_OK; ++i) {
        if (map) map[i] = NULL;
        XrXirType type = (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + i);
        if (!xr_xir_type_is_enum(types, type)) continue;
        const XrXirNominalIdentity *identity = &types->nominals->identities[types->nodes[i].nominal.declaration];
        XirNominalValue **variants = arena_nominal_span(c, identity->variant_count,
            sizeof(*variants), _Alignof(XirNominalValue *));
        if (map) map[i] = variants;
        for (uint32_t v = 0; v < identity->variant_count && c->status == XR_XIR_VALUE_OK; ++v) {
            if (variants) variants[v] = NULL;
            if (identity->variants[v].field_count) continue;
            XirNominalValue *value = arena_nominal_span(c, 1, sizeof(*value), _Alignof(XirNominalValue));
            if (value) {
                atomic_init(&value->object.references, 1);
                value->object.domain = arena->domain; value->object.arena = arena;
                value->object.type = type; value->object.kind = XR_XIR_TYPE_NOMINAL;
                value->object.release_next = NULL;
                value->count = 0; value->variant = v; value->fields = NULL;
                variants[v] = value;
            }
        }
    }
    return map;
}
XR_FUNC const XirNominalValue *xr_xir_type_arena_empty_variant(
    const XrXirTypeArena *arena, XrXirType type, uint32_t variant) {
    if (!arena || !arena->empty_variants || !xr_xir_type_is_enum(&arena->types, type)) return NULL;
    const XrXirTypeNode *node = xr_xir_type_node(&arena->types, type);
    const XrXirNominalIdentity *identity = &arena->types.nominals->identities[node->nominal.declaration];
    if (variant >= identity->variant_count) return NULL;
    return arena->empty_variants[(uint32_t) type - XR_XIR_CONSTRUCTED_TYPE_BASE][variant];
}
