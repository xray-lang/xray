/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_hash_lifetime.inc.c - Typed hash-entry ownership without release allocation
 */
static StorageSpan hash_span(const XirHash *map, size_t index, bool value) {
    return (StorageSpan){value ? map->value_type : map->key_type,
        hash_entry(map, index) + (value ? map->value_offset : map->key_offset)};
}
static uint64_t hash_header_bytes(const XirHash *map) {
    _Static_assert(_Alignof(XirHash) >= _Alignof(StorageFrame), "hash tail aligns ownership frames");
    return sizeof(*map) + (uint64_t)map->release_capacity * sizeof(StorageFrame);
}
static void hash_queue_field(XirHash *map, size_t index, bool value, XirObject **pending) {
        StorageSpan span = hash_span(map, index, value);
        if (span.type == XR_XIR_UNIT) return;
        StorageCursor cursor = {0};
        XR_CHECK(storage_cursor_init(map->object.arena, span,
            map->release_capacity ? (StorageFrame *)(map + 1) : NULL,
            map->release_capacity, true, &cursor) == XR_XIR_VALUE_OK,
            "hash release uses its sealed compact field layouts");
        storage_queue_release(&cursor, UINT64_MAX, pending);
}
static void hash_queue_entry(XirHash *map, size_t index, XirObject **pending) {
    hash_queue_field(map, index, false, pending);
    if (map->object.kind == XR_XIR_TYPE_MAP) hash_queue_field(map, index, true, pending);
}
