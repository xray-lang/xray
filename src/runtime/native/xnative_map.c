/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_map.c - Typed logical Map owners on the canonical hash kernel
 *
 * KEY CONCEPT:
 *   Each guest owner retains one actual Map reference. Mutations commit to
 *   the real binding before draining retired graph owners or callbacks.
 */
#include "xnative_map.h"
#include "xir/xxir_map.h"
#include "xir/xxir_value_internal.h"
#include "xir/xxir_cell_owner_internal.h"
#include "xir/xxir_type_arena_internal.h"
#include "xir/xxir_types.h"
#include "base/xhash.h"
#include <string.h>

struct XrtMapOwner {
    XrXirValue value;
    const XrXirTypeArena *arena;
    uint32_t count;
};

XR_FUNC XrXirValueStatus xrt_map_types_new(uint64_t limit, XrXirTypeArena **output, uint64_t *bytes) {
    if (!output || *output || !bytes) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_FINITE, limit, true};
    if (limit == UINT64_MAX) limits = (XrCompileResourceLimits){XR_COMPILE_MEMORY_ORDINARY, 0, true};
    XrXirCompileContext context = {.limits=xr_xir_compile_default_limits()};
    XrCompileResourceStatus resource = xr_compile_resources_new(&limits, &context.resources);
    if (resource != XR_COMPILE_RESOURCE_OK)
        return resource == XR_COMPILE_RESOURCE_MEMORY_LIMIT ? XR_XIR_VALUE_MEMORY_LIMIT : XR_XIR_VALUE_OOM;
    XrXirTypeNode node = {.kind=XR_XIR_TYPE_MAP, .element=XR_XIR_STRING, .result=XR_XIR_I64};
    XrXirTypes types = {&node, 1, NULL, NULL};
    XrXirValueStatus status = xr_xir_compile_type_arena_new(&context, &types, output);
    XrCompileResourceStats stats = {0};
    if (status == XR_XIR_VALUE_OK) {
        resource = xr_compile_resources_stats(context.resources, &stats);
        if (resource != XR_COMPILE_RESOURCE_OK) {
            xr_xir_compile_type_arena_drop(*output); *output = NULL;
            status = XR_XIR_VALUE_BAD_ARGUMENT;
        } else *bytes = stats.held_bytes;
    }
    xr_compile_resources_release(context.resources);
    return status;
}
static XrXirValue map_value(const XrtMap *map) {
    return map ? (XrXirValue){((const XirObject *)map)->type, 0, (int64_t)(intptr_t)map} : (XrXirValue){0};
}
static bool map_shape(const XrXirValue *value) {
    if (!value->payload || !xr_xir_value_valid(value)) return false;
    const XrXirTypeNode *node = xr_xir_type_node(
        xr_xir_compile_type_arena_types(xr_xir_value_arena(value)), (XrXirType)value->type);
    return node && node->kind == XR_XIR_TYPE_MAP && node->element == XR_XIR_STRING && node->result == XR_XIR_I64;
}
static XrXirValueStatus map_root_types_ensure(XrtValueRoot *root) {
    if (!root) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (root->map_types) return XR_XIR_VALUE_OK;
    uint64_t bytes = 0;
    return xrt_map_types_new(UINT64_MAX, &root->map_types, &bytes);
}
static XrXirValueAdmission map_admission(XrtValueRoot *root, uint32_t source) {
    XrXirValueStatus status = map_root_types_ensure(root);
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
    return (XrXirValueAdmission){root->map_types, xrt_value_root_domain(root, source),
        NULL, NULL, UINT64_MAX, UINT64_MAX};
}
XR_FUNC const XrXirTypeArena *xrt_map_root_types(uint32_t source) {
    const XrXirTypeArena *types = NULL;
    XrXirValueStatus status = xrt_map_root_types_ensure(&types);
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
    return types;
}
XR_FUNC XrXirValueStatus xrt_map_root_types_ensure(const XrXirTypeArena **output) {
    if (!output || *output) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrtValueRoot *root = xrt_value_root_active();
    XrXirValueStatus status = map_root_types_ensure(root);
    if (status == XR_XIR_VALUE_OK) *output = root->map_types;
    return status;
}
XR_FUNC XrXirValueStatus xrt_map_root_bind_types(const XrXirTypeArena *types) {
    XrtValueRoot *root = xrt_value_root_active();
    const XrXirTypes *view = xr_xir_compile_type_arena_types(types);
    const XrXirTypeNode *node = xr_xir_type_node(view, XR_XIR_CONSTRUCTED_TYPE_BASE);
    if (!root || !view || view->count != 1 || !node || node->kind != XR_XIR_TYPE_MAP ||
        node->element != XR_XIR_STRING || node->result != XR_XIR_I64) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (root->map_types)
        return xir_type_arena_same_type(root->map_types, XR_XIR_CONSTRUCTED_TYPE_BASE,
            types, XR_XIR_CONSTRUCTED_TYPE_BASE) ? XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_compile_type_arena_retain((XrXirTypeArena *)types)) return XR_XIR_VALUE_REFCOUNT_LIMIT;
    root->map_types = (XrXirTypeArena *)types;
    return XR_XIR_VALUE_OK;
}
static size_t map_slot(int64_t payload, uint32_t capacity) {
    return xr_hash_bytes(&payload, sizeof(payload)) & (capacity - 1);
}
static XrtMapOwner *map_find(const XrtValueRoot *root, int64_t payload) {
    if (!root->maps) return NULL;
    for (size_t i = map_slot(payload, root->map_capacity);; i = (i + 1) & (root->map_capacity - 1)) {
        XrtMapOwner *owner = &root->maps[i];
        if (owner->value.payload == payload) return owner;
        if (!owner->value.payload) return NULL;
    }
}
static void map_place(XrtValueRoot *root, XrtMapOwner owner) {
    for (size_t i = map_slot(owner.value.payload, root->map_capacity);; i = (i + 1) & (root->map_capacity - 1))
        if (!root->maps[i].value.payload) { root->maps[i] = owner; return; }
}
static void map_reserve(XrtValueRoot *root, uint32_t source) {
    if (root->map_held < root->map_capacity / 2) return;
    if (root->map_capacity > UINT32_MAX / 4) xrt_value_failure(XR_XIR_VALUE_LIMIT, source);
    uint32_t capacity = root->map_capacity ? root->map_capacity * 2 : 16;
    if (sizeof(XrtMapOwner) > SIZE_MAX / capacity) xrt_value_failure(XR_XIR_VALUE_LIMIT, source);
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    XrtMapOwner *table = xr_xir_domain_allocate(xrt_value_root_domain(root, source),
        (size_t)capacity * sizeof(*table), &status);
    if (!table) xrt_value_failure(status, source);
    memset(table, 0, (size_t)capacity * sizeof(*table));
    XrtMapOwner *previous = root->maps;
    uint32_t old_capacity = root->map_capacity;
    root->maps = table; root->map_capacity = capacity;
    for (uint32_t i = 0; i < old_capacity; ++i)
        if (previous[i].value.payload) map_place(root, previous[i]);
    if (previous) xr_xir_domain_deallocate(root->domain, previous, (size_t)old_capacity * sizeof(*previous));
}
static void map_remove_owner(XrtValueRoot *root, XrtMapOwner *owner) {
    size_t hole = (size_t)(owner - root->maps), mask = root->map_capacity - 1;
    for (size_t next = (hole + 1) & mask; root->maps[next].value.payload; next = (next + 1) & mask) {
        size_t home = map_slot(root->maps[next].value.payload, root->map_capacity);
        if (((next - home) & mask) >= ((next - hole) & mask)) {
            root->maps[hole] = root->maps[next]; hole = next;
        }
    }
    root->maps[hole] = (XrtMapOwner){0}; --root->map_count;
}
static void map_adopt(XrtValueRoot *root, XrXirValue value) {
    XrtMapOwner *owner = map_find(root, value.payload);
    if (owner) ++owner->count;
    else { map_place(root, (XrtMapOwner){value, xr_xir_value_arena(&value), 1}); ++root->map_count; }
    ++root->map_held;
}
static XrXirValue map_argument(XrtValueRoot *root, const XrtMap *map,
    XrXirValueAdmission *admission, uint32_t source) {
    XrtMapOwner *owner = map ? map_find(root, (int64_t)(intptr_t)map) : NULL;
    if (owner) { admission->arena = owner->arena; return owner->value; }
    XrXirValue value = map_value(map);
    if (!map_shape(&value)) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    admission->arena = xr_xir_value_arena(&value);
    XrXirValueStatus status = xr_xir_value_admit(&value, (XrXirType)value.type, admission);
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
    return value;
}

XRAY_API XrtMap *xrt_map_string_i64_new(XrtString *const *keys, const int64_t *values,
    size_t count, uint32_t source) {
    XrtValueRoot *root = xrt_value_root_current(source);
    if (count && (!keys || !values)) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    if (count > INT64_MAX || count > SIZE_MAX / (2 * sizeof(XrXirValue)))
        xrt_value_failure(XR_XIR_VALUE_LIMIT, source);
    XR_CHECK(!root->pending.type && !root->pending.payload, "Map construction owns one empty pending result");
    XrXirValueAdmission admission = map_admission(root, source);
    map_reserve(root, source);
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    XrXirValue *input = count ? xr_xir_domain_allocate(root->domain, count * 2 * sizeof(*input), &status) : NULL;
    if (count && !input) xrt_value_failure(status, source);
    for (size_t i = 0; i < count; ++i) {
        input[i] = (XrXirValue){XR_XIR_STRING, 0, (int64_t)(intptr_t)keys[i]};
        input[count + i] = (XrXirValue){XR_XIR_I64, 0, values[i]};
    }
    status = xr_xir_map_new((XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE, input,
        count ? input + count : NULL, count, &admission, &root->pending);
    if (input) xr_xir_domain_deallocate(root->domain, input, count * 2 * sizeof(*input));
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
    return xrt_map_publish_pending(source);
}
XR_FUNC XrtMap *xrt_map_publish_pending(uint32_t source) {
    XrtValueRoot *root = xrt_value_root_current(source);
    if (!map_shape(&root->pending)) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    map_reserve(root, source);
    XrtMap *result = (XrtMap *)(intptr_t)root->pending.payload;
    map_adopt(root, root->pending); root->pending = (XrXirValue){0};
    return result;
}
XRAY_API XrtMap *xrt_map_retain(const XrtMap *map, uint32_t source) {
    XrtValueRoot *root = xrt_value_root_current(source);
    XrXirValueAdmission admission = map_admission(root, source);
    XrXirValue value = map_argument(root, map, &admission, source), copy = {0};
    map_reserve(root, source);
    XrXirValueStatus status = xr_xir_value_copy(&value, &copy);
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
    map_adopt(root, copy);
    return (XrtMap *)(intptr_t)copy.payload;
}
XRAY_API void xrt_map_drop(XrtMap **map) {
    if (!map || !*map) return;
    XrtValueRoot *root = xrt_value_root_current(0);
    XrtMapOwner *owner = map_find(root, (int64_t)(intptr_t)*map);
    XR_CHECK(owner && owner->count, "Map drop consumes one actual guest owner");
    XrXirValue value = owner->value; *map = NULL; --root->map_held;
    if (!--owner->count) map_remove_owner(root, owner);
    xr_xir_value_drop(&value);
}
XRAY_API void xrt_map_assign(XrtMap **destination, const XrtMap *value, uint32_t source) {
    if (!destination) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    XrtMap *copy = xrt_map_retain(value, source);
    XrtMap *previous = *destination;
    *destination = copy;
    xrt_map_drop(&previous);
}
XR_FUNC XrXirValueStatus xrt_map_owned_copy(const XrtMap *map, const XrXirTypeArena *types, XrtMap **output) {
    if (!map || !types || !output || *output) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypeNode *node = xr_xir_type_node(xr_xir_compile_type_arena_types(types),
        (XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE);
    XrXirValue value = map_value(map), copy = {0};
    if (!node || node->kind != XR_XIR_TYPE_MAP || node->element != XR_XIR_STRING ||
        node->result != XR_XIR_I64 || !map_shape(&value)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueAdmission admission = {xr_xir_value_arena(&value), ((const XirObject *)map)->domain,
        NULL, NULL, UINT64_MAX, UINT64_MAX};
    XrXirValueStatus status = xr_xir_value_admit(&value, (XrXirType)value.type, &admission);
    if (status == XR_XIR_VALUE_OK) status = xr_xir_value_copy(&value, &copy);
    if (status == XR_XIR_VALUE_OK) *output = (XrtMap *)(intptr_t)copy.payload;
    return status;
}
XRAY_API void xrt_map_owned_drop(XrtMap **map) {
    if (!map || !*map) return;
    XrXirValue value = map_value(*map); *map = NULL;
    xr_xir_value_drop(&value);
}
XR_FUNC XrtMap *xrt_map_root_take(XrtMap *map) {
    XrtValueRoot *root = xrt_value_root_current(0);
    XrtMapOwner *owner = map_find(root, (int64_t)(intptr_t)map);
    XR_CHECK(owner && owner->count, "Map result transfer owns one registered actual reference");
    --root->map_held;
    if (!--owner->count) map_remove_owner(root, owner);
    return map;
}
XRAY_API int64_t xrt_map_length(const XrtMap *map, uint32_t source) {
    XrtValueRoot *root = xrt_value_root_current(source);
    XrXirValueAdmission admission = map_admission(root, source);
    XrXirValue value = map_argument(root, map, &admission, source);
    int64_t length = 0;
    XrXirValueStatus status = xr_xir_hash_len(&value, &admission, &length);
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
    return length;
}
XRAY_API bool xrt_map_string_i64_get(const XrtMap *map, const XrtString *key, int64_t *output, uint32_t source) {
    if (!output) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    XrtValueRoot *root = xrt_value_root_current(source);
    XrXirValueAdmission admission = map_admission(root, source);
    XrXirValue value = map_argument(root, map, &admission, source);
    XrXirValue input = {XR_XIR_STRING, 0, (int64_t)(intptr_t)key}, result = {0};
    bool found = false;
    XrXirValueStatus status = xr_xir_map_get(&value, &input, &admission, &result, &found);
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
    *output = found ? result.payload : 0;
    return found;
}
XRAY_API bool xrt_map_string_i64_contains(const XrtMap *map, const XrtString *key, uint32_t source) {
    XrtValueRoot *root = xrt_value_root_current(source);
    XrXirValueAdmission admission = map_admission(root, source);
    XrXirValue value = map_argument(root, map, &admission, source);
    XrXirValue input = {XR_XIR_STRING, 0, (int64_t)(intptr_t)key}; bool found = false;
    XrXirValueStatus status = xr_xir_map_contains_key(&value, &input, &admission, &found);
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
    return found;
}
XRAY_API int64_t xrt_map_string_i64_index(const XrtMap *map, const XrtString *key, uint32_t source) {
    int64_t value = 0;
    if (!xrt_map_string_i64_get(map, key, &value, source)) xrt_typed_panic(431, source);
    return value;
}
typedef enum MapMutation { MAP_SET, MAP_REMOVE, MAP_CLEAR } MapMutation;
static bool map_mutate(XrtMap **binding, bool registered, const XrtString *key,
    int64_t number, MapMutation operation, uint32_t source) {
    XrtValueRoot *root = xrt_value_root_current(source);
    if (!binding || !*binding) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    XrXirValueAdmission admission = map_admission(root, source);
    XrXirValue value = map_argument(root, *binding, &admission, source);
    int64_t previous = value.payload;
    if (registered) {
        XrtMapOwner *owner = map_find(root, previous);
        if (!owner || !owner->count) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
        map_reserve(root, source);
    }
    XrXirValue input = {XR_XIR_STRING, 0, (int64_t)(intptr_t)key};
    XrXirValue replacement = {XR_XIR_I64, 0, number}; bool removed = false;
    XrXirValue retired = {0};
    XrXirValuePlace place = {(XrXirType)value.type, binding, NULL};
    xr_xir_value_graph_begin();
    XrXirValueStatus status = operation == MAP_SET ?
        xr_xir_map_set_deferred(&place, &input, &replacement, &admission, &retired) :
        operation == MAP_REMOVE ? xr_xir_hash_delete_deferred(&place, &input, &admission, &removed, &retired) :
        xr_xir_hash_clear_deferred(&place, &admission, &retired);
    if (status == XR_XIR_VALUE_OK && registered && (int64_t)(intptr_t)*binding != previous) {
        XrtMapOwner *owner = map_find(root, previous);
        --root->map_held;
        if (!--owner->count) map_remove_owner(root, owner);
        map_adopt(root, map_value(*binding));
    }
    xr_xir_value_drop(&retired);
    xr_xir_value_graph_end();
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
    return removed;
}
XRAY_API void xrt_map_string_i64_set(XrtMap **map, const XrtString *key, int64_t value, uint32_t source) {
    (void)map_mutate(map, true, key, value, MAP_SET, source);
}
XRAY_API bool xrt_map_string_i64_remove(XrtMap **map, const XrtString *key, uint32_t source) {
    return map_mutate(map, true, key, 0, MAP_REMOVE, source);
}
XRAY_API void xrt_map_clear(XrtMap **map, uint32_t source) {
    (void)map_mutate(map, true, NULL, 0, MAP_CLEAR, source);
}
#include "xnative_map_ref.inc.c"
XR_FUNC void xrt_map_root_drain(XrtValueRoot *root) {
    for (uint32_t i = 0; i < root->map_capacity; ++i) {
        XrtMapOwner owner = root->maps[i];
        for (uint32_t count = owner.count; count; --count) {
            XrXirValue value = owner.value;
            xr_xir_value_drop(&value);
        }
    }
    if (root->maps) xr_xir_domain_deallocate(root->domain, root->maps,
        (size_t)root->map_capacity * sizeof(*root->maps));
    xr_xir_compile_type_arena_drop(root->map_types);
    root->maps = NULL; root->map_types = NULL;
    root->map_capacity = root->map_count = 0; root->map_held = 0;
}
