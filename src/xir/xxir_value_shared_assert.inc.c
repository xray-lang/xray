/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_value_shared_assert.inc.c - Verify committed shared ownership edges
 *
 * KEY CONCEPT:
 *   Inspect actual installed values before retiring their previous owners.
 *   Packed fields reuse the write transaction's existing traversal workspace.
 */
static bool shared_edge_assert(const XrXirValue *value, void *payload, void *context) {
    (void)payload;
    (void)context;
    if (owned_carrier_type((XrXirType)value->type)) {
        XirObject *child = object_pointer(value);
        XR_CHECK(empty_enum_object(child) || xir_object_references(child) < 0,
            "a committed child of a shared identity remains in the shared reference band");
    }
    return true;
}
static void shared_child_assert(const XirObject *parent, const XrXirValue *value) {
    if (parent && xir_object_references(parent) < 0)
        (void)shared_edge_assert(value, NULL, NULL);
}
static void shared_storage_assert(const XirObject *parent, const XrXirTypeArena *arena,
    StorageSpan span, StorageFrame *frames, uint32_t capacity) {
    if (!parent || xir_object_references(parent) >= 0) return;
    StorageCursor cursor = {0};
    XR_CHECK(storage_cursor_init(arena, span, frames, capacity, true, &cursor) == XR_XIR_VALUE_OK,
        "a committed packed field retains its prepared ownership traversal");
    XR_CHECK(graph_storage_edges(&cursor, shared_edge_assert, NULL, NULL),
        "a committed shared field has a complete direct ownership view");
}
