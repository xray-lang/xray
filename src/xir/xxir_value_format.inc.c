/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_value_format.inc.c - Allocation-free display borrows of actual storage
 *
 * KEY CONCEPT:
 *   Packed children use sealed offsets and the canonical scalar decoder.
 *   Borrowing never materializes an owned snapshot or acquires a reference.
 */
typedef struct FormatStorage {
    XrValueFormatNode node;
    XrXirValue leaf;
    const XrXirTypeNode *type;
    const XrXirStorageLayout *layout;
    const XrXirTypeArena *arena;
    uint32_t variant;
    bool none;
} FormatStorage;

static bool format_storage_resolve(const XrXirTypeArena *arena,
    XrValueFormatNode node, FormatStorage *out) {
    if (!arena) return false;
    *out = (FormatStorage){0};
    for (uint32_t hop = 0; hop < XR_XIR_CONSTRUCTED_TYPE_LIMIT; ++hop) {
        if (node.owner) arena = node.owner;
        if (!node.type) {
            const XrXirValue *value = node.value;
            if (!xr_xir_value_valid(value)) return false;
            out->leaf = *value;
            const XrXirTypeArena *owner = xr_xir_value_arena(value);
            if (owner) arena = owner;
            const XrXirTypes *types = xr_xir_compile_type_arena_types(arena);
            bool some = false; const XrXirValue *payload = NULL;
            if (owner && xr_xir_type_is_nullable(types, (XrXirType)value->type)) {
                if (!xr_xir_nullable_view(value, &some, &payload)) return false;
                if (!some) { out->none = true; out->node = node; out->arena = arena; return true; }
                node = (XrValueFormatNode){payload, 0, arena}; continue;
            }
            out->node = node; out->arena = arena; return true;
        }
        const XrXirTypes *types = xr_xir_compile_type_arena_types(arena);
        XrXirType type = (XrXirType)node.type;
        XrXirLayout physical = {0};
        if (!xr_xir_compile_type_arena_layout(arena, type, &physical) ||
            (physical.size && !node.value)) return false;
        const XrXirTypeNode *descriptor = xr_xir_type_node(types, type);
        const XrXirStorageLayout *layout = xr_xir_compile_type_arena_storage(arena, type);
        if (descriptor && descriptor->kind == XR_XIR_TYPE_NULLABLE) {
            if (!layout || !node.value || *(const unsigned char *)node.value > 1) return false;
            if (!*(const unsigned char *)node.value) { out->none = true; out->node = node; out->arena = arena; return true; }
            node = (XrValueFormatNode){(const unsigned char *)node.value + layout->value.alignment,
                (uint16_t)descriptor->element, arena}; continue;
        }
        if (descriptor && xr_xir_type_is_enum(types, type)) {
            if (!layout) return false;
            uint32_t variant = 0;
            if (layout->tag_bytes) memcpy(&variant, node.value, layout->tag_bytes);
            const XrXirNominalIdentity *identity = &types->nominals->identities[descriptor->nominal.declaration];
            if (variant >= identity->variant_count) return false;
            out->node = node; out->type = descriptor; out->layout = layout; out->variant = variant;
            out->arena = arena;
            return true;
        }
        if (!physical.size || physical.size > sizeof(uint64_t) || inline_nominal_type(types, type)) return false;
        if (!storage_leaf_value(arena, type, node.value, physical.size, &out->leaf)) return false;
        if (!xr_xir_value_argument(&out->leaf, arena, type)) return false;
        out->arena = xr_xir_value_arena(&out->leaf);
        if (!out->arena) out->arena = arena;
        out->node = node; return true;
    }
    return false;
}

XR_FUNC bool xr_xir_value_format_storage(const void *context, XrValueFormatNode node,
    XrXirValue *leaf, XrValueFormatView *view) {
    const XrXirTypeArena *arena = context;
    FormatStorage storage;
    if (!leaf || !view || !format_storage_resolve(arena, node, &storage)) return false;
    *leaf = storage.leaf;
    if (storage.none) {
        view->kind = XR_VALUE_FORMAT_LITERAL; view->name = "null"; view->name_size = 4; return true;
    }
    const XrXirTypes *types = xr_xir_compile_type_arena_types(storage.arena);
    if (storage.type) {
        const XrXirNominalIdentity *identity = &types->nominals->identities[storage.type->nominal.declaration];
        const XrXirNominalVariant *variant = &identity->variants[storage.variant];
        view->kind = XR_VALUE_FORMAT_ENUM;
        view->name = identity->name.bytes; view->name_size = identity->name.length;
        view->member = variant->name.bytes; view->member_size = variant->name.length;
        view->children = variant->field_count; return true;
    }
    if (xr_xir_type_is_array(types, (XrXirType)leaf->type)) {
        const XirArray *array = (const XirArray *)object_pointer(leaf);
        view->kind = XR_VALUE_FORMAT_ARRAY; view->size = array->length; return true;
    }
    if (xr_xir_type_is_enum(types, (XrXirType)leaf->type)) {
        XrXirEnumBorrow borrowed;
        if (xr_xir_enum_borrow(leaf, &borrowed) != XR_XIR_VALUE_OK) return false;
        view->kind = XR_VALUE_FORMAT_ENUM;
        view->name = borrowed.name.bytes; view->name_size = borrowed.name.length;
        view->member = borrowed.member.bytes; view->member_size = borrowed.member.length;
        view->children = borrowed.field_count;
    }
    return true;
}

XR_FUNC int xr_xir_value_format_child(const void *context, XrValueFormatNode node,
    uint32_t index, XrValueFormatNode *child) {
    const XrXirTypeArena *arena = context;
    FormatStorage storage;
    if (!child || !format_storage_resolve(arena, node, &storage) || storage.none) return 0;
    const XrXirTypes *types = xr_xir_compile_type_arena_types(storage.arena);
    if (storage.type) {
        const XrXirNominalIdentity *identity = &types->nominals->identities[storage.type->nominal.declaration];
        const XrXirNominalVariant *variant = &identity->variants[storage.variant];
        if (index >= variant->field_count) return 0;
        uint32_t field = variant->field_begin + index;
        *child = (XrValueFormatNode){storage.node.value ? (const unsigned char *)storage.node.value +
            storage.layout->field_offsets[field] : NULL, (uint16_t)storage.type->nominal.fields[field], storage.arena};
        return 1;
    }
    if (xr_xir_type_is_array(types, (XrXirType)storage.leaf.type)) {
        const XirArray *array = (const XirArray *)object_pointer(&storage.leaf);
        if (index >= array->length) return 0;
        *child = (XrValueFormatNode){array->data ? array->data + (size_t)index * array->stride : NULL,
            (uint16_t)array->element, array->object.arena}; return 1;
    }
    XrXirEnumBorrow borrowed;
    if (xr_xir_enum_borrow(&storage.leaf, &borrowed) != XR_XIR_VALUE_OK || index >= borrowed.field_count) return 0;
    *child = (XrValueFormatNode){&borrowed.fields[index], 0, storage.arena}; return 1;
}
