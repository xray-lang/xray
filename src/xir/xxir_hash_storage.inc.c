/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_hash_storage.inc.c - Sealed dense entries and indirect hash buckets
 *
 * KEY CONCEPT:
 *   Buckets name dense entries, so ownership traversal ignores spare capacity.
 *   Keys and values use their own compact target-bound storage layouts.
 */
typedef struct HashLayout {
    XrXirType key, value;
    uint32_t key_offset, value_offset, stride, release_capacity;
} HashLayout;
static XrXirValueStatus hash_layout(const XrXirTypeArena *arena, XrXirType type, HashLayout *out) {
    const XrXirTypeNode *node = xr_xir_type_node(xr_xir_compile_type_arena_types(arena), type);
    if (!node || node->parameter_span ||
        (node->kind != XR_XIR_TYPE_MAP && node->kind != XR_XIR_TYPE_SET)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_key_builtin_type(node->element)) return XR_XIR_VALUE_UNSUPPORTED;
    XrXirType value = node->kind == XR_XIR_TYPE_MAP ? node->result : XR_XIR_UNIT;
    XrXirLayout key_layout = {0}, value_layout = {0};
    if (!xr_xir_compile_type_arena_layout(arena, node->element, &key_layout) ||
        !xr_xir_compile_type_arena_layout(arena, value, &value_layout) ||
        !key_layout.alignment || !value_layout.alignment || key_layout.alignment > 8 || value_layout.alignment > 8)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    uint64_t key_offset = (UINT64_C(8) + key_layout.alignment - 1) & ~(uint64_t)(key_layout.alignment - 1);
    uint64_t value_offset = (key_offset + key_layout.size + value_layout.alignment - 1) &
        ~(uint64_t)(value_layout.alignment - 1);
    uint64_t stride = (value_offset + value_layout.size + 7) & ~UINT64_C(7);
    if (stride > UINT32_MAX) return XR_XIR_VALUE_LIMIT;
    uint32_t depth = array_release_depth(arena, value);
    *out = (HashLayout){node->element, value, (uint32_t)key_offset, (uint32_t)value_offset, (uint32_t)stride, depth};
    return XR_XIR_VALUE_OK;
}
static bool hash_data_size(size_t capacity, uint32_t stride, size_t *out) {
    uint64_t unit = (uint64_t)stride + sizeof(uint64_t);
    if (capacity > SIZE_MAX / unit) return false;
    *out = (size_t)(capacity * unit); return true;
}
static bool hash_storage_valid(const XirHash *map) {
    HashLayout layout = {0}; size_t bytes = 0;
    return hash_layout(map->object.arena, map->object.type, &layout) == XR_XIR_VALUE_OK &&
        map->key_type == layout.key && map->value_type == layout.value &&
        map->key_offset == layout.key_offset && map->value_offset == layout.value_offset &&
        map->stride == layout.stride && map->release_capacity == layout.release_capacity &&
        ((!map->capacity && !map->length) || (map->capacity >= 8 &&
         !(map->capacity & (map->capacity - 1)) && map->length <= map->capacity / 2)) &&
        map->length <= INT64_MAX && hash_data_size(map->capacity, map->stride, &bytes) &&
        ((map->capacity != 0) == (map->data != NULL));
}
static uint64_t *hash_buckets(const XirHash *map) { return (uint64_t *)map->data; }
static unsigned char *hash_entry(const XirHash *map, size_t index) {
    XR_CHECK(index < map->capacity, "hash entry requires reserved dense capacity");
    return map->data + map->capacity * sizeof(uint64_t) + index * map->stride;
}
