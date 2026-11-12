/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_value_share.inc.c - Complete graph discovery before shared RC publication
 *
 * KEY CONCEPT:
 *   Only local objects lend scratch links under the residual graph lock.
 *   Every fallible root-domain pin precedes the first reference-band change.
 */
typedef struct ObjectShare {
    XirObject *first, *last;
    XrXirValueStatus status;
} ObjectShare;

static bool object_share_edge(const XrXirValue *value, void *payload, void *context) {
    (void)payload;
    ObjectShare *share = context;
    if (!xr_xir_value_valid(value)) {
        share->status = XR_XIR_VALUE_BAD_ARGUMENT;
        return false;
    }
    if (!owned_carrier_type((XrXirType)value->type)) return true;
    XirObject *object = object_pointer(value);
    if (empty_enum_object(object) || xir_object_references(object) < 0 || object->graph_work == object)
        return true;
    object->graph_work = object;
    object->release_next = NULL;
    if (share->last) share->last->release_next = object;
    else share->first = object;
    share->last = object;
    return true;
}
static XrXirValueStatus object_share_finish(ObjectShare *share) {
    for (XirObject *object = share->first; share->status == XR_XIR_VALUE_OK && object; object = object->release_next) {
        /* Each Channel independently publishes its queued payloads. Its
         * synchronized identity needs no concurrent queue traversal here. */
        if (object->kind != XR_XIR_TYPE_CHANNEL && !graph_edges(object, object_share_edge, share) &&
            share->status == XR_XIR_VALUE_OK) share->status = XR_XIR_VALUE_BAD_ARGUMENT;
    }
    for (XirObject *object = share->first; share->status == XR_XIR_VALUE_OK && object; object = object->release_next)
        if (!root_string_escape(object)) share->status = XR_XIR_VALUE_REFCOUNT_LIMIT;
    for (XirObject *object = share->first; object;) {
        XirObject *next = object->release_next;
        if (share->status == XR_XIR_VALUE_OK) {
            int32_t count = xir_object_references(object);
            XR_CHECK(count > 0, "the publisher owns every discovered local reference");
            atomic_store_explicit(&object->references, -count, memory_order_release);
        }
        object->graph_work = NULL;
        object->release_next = NULL;
        object = next;
    }
    return share->status;
}
XR_FUNC XrXirValueStatus xr_xir_value_share(const XrXirValue *source) {
    ObjectShare share = {.status=XR_XIR_VALUE_OK};
    xr_xir_value_graph_begin();
    graph_lock();
    (void)object_share_edge(source, NULL, &share);
    XrXirValueStatus status = object_share_finish(&share);
    graph_unlock();
    xr_xir_value_graph_end();
    return status;
}
static XrXirValueStatus object_share_storage(const XrXirTypeArena *arena, StorageSpan span,
    StorageFrame *frames, uint32_t capacity) {
    ObjectShare share = {.status=XR_XIR_VALUE_OK};
    StorageCursor cursor = {0};
    share.status = storage_cursor_init(arena, span, frames, capacity, true, &cursor);
    if (share.status != XR_XIR_VALUE_OK) return share.status;
    xr_xir_value_graph_begin();
    graph_lock();
    if (!graph_storage_edges(&cursor, object_share_edge, &share, NULL) && share.status == XR_XIR_VALUE_OK)
        share.status = XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status = object_share_finish(&share);
    graph_unlock();
    xr_xir_value_graph_end();
    return status;
}
static bool value_place_shared(const XrXirValuePlace *place) {
    return place && place->identity && xir_object_references(place->identity) < 0;
}
static XrXirValueStatus value_place_publish(const XrXirValuePlace *place, const XrXirValue *value) {
    return value_place_shared(place) ? xr_xir_value_share(value) : XR_XIR_VALUE_OK;
}
