/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_storage_cursor.inc.c - Borrowed active-field traversal with reserved frames
 *
 * KEY CONCEPT:
 *   Inline descent uses caller-owned frames; object release reuses its worklist.
 */
typedef struct StorageSpan {
    XrXirType type;
    const unsigned char *bytes;
} StorageSpan;
typedef struct StorageFrame {
    StorageSpan span;
    uint32_t next, end;
} StorageFrame;
typedef struct StorageCursor {
    const XrXirTypeArena *arena;
    StorageFrame *frames;
    StorageSpan pending;
    uint32_t depth, capacity;
    bool has_pending, owned_only;
} StorageCursor;

static XrXirValueStatus storage_cursor_init(const XrXirTypeArena *arena, StorageSpan span,
    StorageFrame *frames, uint32_t capacity, bool owned_only, StorageCursor *cursor) {
    if (!cursor) return XR_XIR_VALUE_BAD_ARGUMENT;
    *cursor = (StorageCursor) {0};
    XrXirLayout physical = {0};
    if ((capacity && !frames) || !xr_xir_type_arena_layout(arena, span.type, &physical) ||
        (physical.size && !span.bytes)) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirStorageLayout *layout = xr_xir_type_arena_storage(arena, span.type);
    uint32_t required = layout && inline_nominal_type(xr_xir_type_arena_types(arena), span.type)
        ? (owned_only ? layout->owned_depth : layout->depth) : 0;
    if (capacity < required) return XR_XIR_VALUE_LIMIT;
    *cursor = (StorageCursor) {arena, frames, span, 0, capacity, true, owned_only};
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus storage_cursor_enter(StorageCursor *cursor, StorageSpan span) {
    const XrXirTypes *types = xr_xir_type_arena_types(cursor->arena);
    const XrXirTypeNode *node = xr_xir_type_node(types, span.type);
    const XrXirStorageLayout *layout = xr_xir_type_arena_storage(cursor->arena, span.type);
    XR_CHECK(node && inline_nominal_type(types, span.type) && layout, "inline cursor requires sealed value metadata");
    if (cursor->owned_only && !layout->owned_depth) return XR_XIR_VALUE_OK;
    uint32_t begin = 0, count = node->nominal.field_count;
    if (node->kind == XR_XIR_TYPE_NULLABLE) {
        if (!span.bytes || span.bytes[0] > 1) return XR_XIR_VALUE_BAD_ARGUMENT;
        count = span.bytes[0];
    } else if (types->nominals->identities[node->nominal.declaration].kind == XR_XIR_NOMINAL_ENUM) {
        const XrXirNominalIdentity *identity = &types->nominals->identities[node->nominal.declaration];
        uint32_t variant = 0;
        if (layout->tag_bytes) memcpy(&variant, span.bytes, layout->tag_bytes);
        if (variant >= identity->variant_count) return XR_XIR_VALUE_BAD_ARGUMENT;
        begin = identity->variants[variant].field_begin; count = identity->variants[variant].field_count;
    }
    if (count) {
        if (cursor->depth >= cursor->capacity) return XR_XIR_VALUE_LIMIT;
        cursor->frames[cursor->depth++] = (StorageFrame) {span, begin, begin + count};
    }
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus storage_cursor_next(StorageCursor *cursor, uint64_t *work,
    StorageSpan *leaf, bool *found) {
    if (!cursor || !leaf || !found) return XR_XIR_VALUE_BAD_ARGUMENT;
    *found = false; *leaf = (StorageSpan) {0};
    const XrXirTypes *types = xr_xir_type_arena_types(cursor->arena);
    while (cursor->has_pending || cursor->depth) {
        if (work) {
            if (!*work) return XR_XIR_VALUE_LIMIT;
            --*work;
        }
        StorageSpan span = cursor->pending;
        if (cursor->has_pending) cursor->has_pending = false;
        else {
            StorageFrame *frame = &cursor->frames[cursor->depth - 1];
            if (frame->next == frame->end) { --cursor->depth; continue; }
            const XrXirTypeNode *node = xr_xir_type_node(types, frame->span.type);
            const XrXirStorageLayout *layout = xr_xir_type_arena_storage(cursor->arena, frame->span.type);
            uint32_t field = frame->next++;
            span.type = node->kind == XR_XIR_TYPE_NULLABLE ? node->element : node->nominal.fields[field];
            span.bytes = frame->span.bytes ? frame->span.bytes +
                (node->kind == XR_XIR_TYPE_NULLABLE ? layout->value.alignment : layout->field_offsets[field]) : NULL;
        }
        if (inline_nominal_type(types, span.type)) {
            XrXirValueStatus status = storage_cursor_enter(cursor, span);
            if (status != XR_XIR_VALUE_OK) return status;
        } else if (!cursor->owned_only || owned_carrier_type(span.type)) {
            *leaf = span; *found = true; return XR_XIR_VALUE_OK;
        }
    }
    return XR_XIR_VALUE_OK;
}
/* Only initialized owners may reach this path. A finite prefix stops before
 * any later field or enum tag is read, including during failed construction. */
static void storage_queue_release(StorageCursor *cursor, uint64_t owners, XirObject **pending) {
    XR_CHECK(cursor && cursor->owned_only && pending, "storage release needs an ownership cursor");
    uint64_t released = 0;
    while (released < owners) {
        StorageSpan leaf = {0}; bool found = false;
        XrXirValueStatus status = storage_cursor_next(cursor, NULL, &leaf, &found);
        XR_CHECK(status == XR_XIR_VALUE_OK, "invalid initialized inline storage during release");
        if (!found) break;
        XrXirValue value = {(uint32_t) leaf.type, 0, 0};
        memcpy(&value.payload, leaf.bytes, sizeof(value.payload));
        queue_release(&value, pending); ++released;
    }
    XR_CHECK(owners == UINT64_MAX || released == owners, "storage ownership prefix is incomplete");
}
