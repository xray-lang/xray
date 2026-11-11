/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_array.c - Logical owners of the canonical Array carrier
 *
 * KEY CONCEPT:
 *   Every guest owner contributes a real canonical reference. Registry space
 *   covers the number of logical owners, so a successful COW commit never
 *   needs a subsequent allocation to record its replacement body.
 */
#include "xnative_array.h"
#include "xnative_string.h"
#include "xir/xxir_array.h"
#include "xir/xxir_value_internal.h"
#include "xir/xxir_types.h"
#include "base/xhash.h"
#include <string.h>

struct XrtArrayOwner {
    XrXirValue value;
    const XrXirTypeArena *arena;
    uint32_t count;
    XrtArrayElement element;
};

XR_FUNC XrXirType xrt_array_type(XrtArrayElement element) {
    return (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + (uint32_t)element);
}

XR_FUNC XrXirValueStatus xrt_array_types_new(uint64_t limit, XrXirTypeArena **output, uint64_t *bytes) {
    if (!output || *output || !bytes) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_FINITE, limit, true};
    if (limit == UINT64_MAX) limits = (XrCompileResourceLimits){XR_COMPILE_MEMORY_ORDINARY, 0, true};
    XrXirCompileContext context = {.limits=xr_xir_compile_default_limits()};
    XrCompileResourceStatus resource = xr_compile_resources_new(&limits, &context.resources);
    if (resource != XR_COMPILE_RESOURCE_OK)
        return resource == XR_COMPILE_RESOURCE_MEMORY_LIMIT ? XR_XIR_VALUE_MEMORY_LIMIT : XR_XIR_VALUE_OOM;
    XrXirTypeNode nodes[] = {{.kind=XR_XIR_TYPE_ARRAY, .element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_ARRAY, .element=XR_XIR_STRING}};
    XrXirTypes types = {nodes, 2, NULL, NULL};
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

static XrXirValue array_value(const XrtArray *array) {
    const XirObject *object = (const XirObject *)array;
    return array ? (XrXirValue){(uint32_t)object->type, 0, (int64_t)(intptr_t)array} : (XrXirValue){0};
}
static bool array_element(const XrXirValue *value, XrtArrayElement *element) {
    if (!value->payload || !xr_xir_value_valid(value)) return false;
    const XrXirTypeArena *arena = xr_xir_value_arena(value);
    const XrXirTypeNode *node = xr_xir_type_node(xr_xir_compile_type_arena_types(arena), (XrXirType)value->type);
    if (!node || node->kind != XR_XIR_TYPE_ARRAY ||
        (node->element != XR_XIR_I64 && node->element != XR_XIR_STRING)) return false;
    *element = node->element == XR_XIR_I64 ? XRT_ARRAY_I64 : XRT_ARRAY_STRING;
    return true;
}
/* These two closed builtin element families have structural runtime meaning.
 * Other constructed values retain the canonical arena's identity rules. The
 * actual Array keeps its original type ID, arena and allocation domain. */
static bool array_shape(const XrXirValue *value, const XrXirTypeArena *receiver, XrtArrayElement expected) {
    XrtArrayElement actual = XRT_ARRAY_I64;
    const XrXirTypeNode *node = xr_xir_type_node(xr_xir_compile_type_arena_types(receiver), xrt_array_type(expected));
    return (unsigned)expected <= XRT_ARRAY_STRING && array_element(value, &actual) && actual == expected &&
        node && node->kind == XR_XIR_TYPE_ARRAY &&
        node->element == (expected == XRT_ARRAY_I64 ? XR_XIR_I64 : XR_XIR_STRING);
}
static XrXirValueAdmission array_admission(XrtValueRoot *root, uint32_t source_id) {
    if (!root->types) {
        uint64_t bytes = 0;
        XrXirValueStatus status = xrt_array_types_new(UINT64_MAX, &root->types, &bytes);
        if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source_id);
    }
    return (XrXirValueAdmission){root->types, xrt_value_root_domain(root, source_id),
        NULL, NULL, UINT64_MAX, UINT64_MAX};
}
XR_FUNC XrXirValueStatus xrt_value_root_share_types(const XrXirTypeArena *types) {
    XrtValueRoot *root = xrt_value_root_active();
    if (!root || !types || root->types) return XR_XIR_VALUE_OK;
    if (!xr_xir_compile_type_arena_retain((XrXirTypeArena *)types)) return XR_XIR_VALUE_REFCOUNT_LIMIT;
    root->types = (XrXirTypeArena *)types;
    return XR_XIR_VALUE_OK;
}
static size_t array_slot(int64_t payload, uint32_t capacity) {
    return xr_hash_bytes(&payload, sizeof(payload)) & (capacity - 1);
}
static XrtArrayOwner *array_find(const XrtValueRoot *root, int64_t payload) {
    if (!root->arrays) return NULL;
    for (size_t i = array_slot(payload, root->array_capacity);; i = (i + 1) & (root->array_capacity - 1)) {
        XrtArrayOwner *owner = &root->arrays[i];
        if (owner->value.payload == payload) return owner;
        if (!owner->value.payload) return NULL;
    }
}
static void array_place(XrtValueRoot *root, XrtArrayOwner owner) {
    for (size_t i = array_slot(owner.value.payload, root->array_capacity);; i = (i + 1) & (root->array_capacity - 1))
        if (!root->arrays[i].value.payload) { root->arrays[i] = owner; return; }
}
static void array_reserve(XrtValueRoot *root, uint32_t source_id) {
    if (root->array_held < root->array_capacity / 2) return;
    if (root->array_capacity > UINT32_MAX / 4) xrt_value_failure(XR_XIR_VALUE_LIMIT, source_id);
    uint32_t capacity = root->array_capacity ? root->array_capacity * 2 : 16;
    if (sizeof(XrtArrayOwner) > SIZE_MAX / capacity) xrt_value_failure(XR_XIR_VALUE_LIMIT, source_id);
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    XrtArrayOwner *table = xr_xir_domain_allocate(xrt_value_root_domain(root, source_id),
        (size_t)capacity * sizeof(*table), &status);
    if (!table) xrt_value_failure(status, source_id);
    memset(table, 0, (size_t)capacity * sizeof(*table));
    XrtArrayOwner *previous = root->arrays;
    uint32_t old_capacity = root->array_capacity;
    root->arrays = table; root->array_capacity = capacity;
    for (uint32_t i = 0; i < old_capacity; ++i)
        if (previous[i].value.payload) array_place(root, previous[i]);
    if (previous) xr_xir_domain_deallocate(root->domain, previous, (size_t)old_capacity * sizeof(*previous));
}
static void array_remove(XrtValueRoot *root, XrtArrayOwner *owner) {
    size_t hole = (size_t)(owner - root->arrays), mask = root->array_capacity - 1;
    for (size_t next = (hole + 1) & mask; root->arrays[next].value.payload; next = (next + 1) & mask) {
        size_t home = array_slot(root->arrays[next].value.payload, root->array_capacity);
        if (((next - home) & mask) >= ((next - hole) & mask)) {
            root->arrays[hole] = root->arrays[next]; hole = next;
        }
    }
    root->arrays[hole] = (XrtArrayOwner){0};
    --root->array_count;
}
static void array_adopt(XrtValueRoot *root, XrXirValue value, XrtArrayElement element) {
    XrtArrayOwner *owner = array_find(root, value.payload);
    if (owner) ++owner->count;
    else {
        array_place(root, (XrtArrayOwner){value, xr_xir_value_arena(&value), 1, element});
        ++root->array_count;
    }
    ++root->array_held;
}
static bool array_current_element(const XrtValueRoot *root, const XrtArray *array, XrtArrayElement *element) {
    XrtArrayOwner *owner = array ? array_find(root, (int64_t)(intptr_t)array) : NULL;
    if (owner) { *element = owner->element; return true; }
    XrXirValue value = array_value(array);
    return array_element(&value, element);
}
static XrXirValue array_argument(XrtValueRoot *root, const XrtArray *array, XrXirValueAdmission *admission,
    XrtArrayElement element, uint32_t source_id) {
    XrtArrayOwner *owner = array ? array_find(root, (int64_t)(intptr_t)array) : NULL;
    if (owner) {
        if (owner->element != element) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source_id);
        /* Registration admitted the actual closed shape. Its canonical owner
         * keeps this immutable arena alive through every registered reference. */
        admission->arena = owner->arena;
        return owner->value;
    }
    XrXirValue value = array_value(array);
    if (!array_shape(&value, admission->arena, element)) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source_id);
    admission->arena = xr_xir_value_arena(&value);
    XrXirValueStatus status = xr_xir_value_admit(&value, (XrXirType)value.type, admission);
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source_id);
    return value;
}

static XrtArray *array_new(const void *values, size_t count, XrtArrayElement element, uint32_t source_id) {
    XrtValueRoot *root = xrt_value_root_current(source_id);
    if (count && !values)
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source_id);
    if (count > INT64_MAX || count > SIZE_MAX / sizeof(XrXirValue))
        xrt_value_failure(XR_XIR_VALUE_LIMIT, source_id);
    XR_CHECK(!root->pending.type && !root->pending.payload, "Array construction starts with an empty pending owner");
    XrXirValueAdmission admission = array_admission(root, source_id);
    array_reserve(root, source_id);
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    XrXirValue *input = count ? xr_xir_domain_allocate(root->domain, count * sizeof(*input), &status) : NULL;
    if (count && !input) xrt_value_failure(status, source_id);
    for (size_t i = 0; i < count; ++i)
        input[i] = element == XRT_ARRAY_I64 ? (XrXirValue){XR_XIR_I64, 0, ((const int64_t *)values)[i]} :
            (XrXirValue){XR_XIR_STRING, 0, (int64_t)(intptr_t)((XrtString *const *)values)[i]};
    status = xr_xir_array_new(xrt_array_type(element), input, count, &admission, &root->pending);
    if (input) xr_xir_domain_deallocate(root->domain, input, count * sizeof(*input));
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source_id);
    XrtArray *array = (XrtArray *)(intptr_t)root->pending.payload;
    array_adopt(root, root->pending, element); root->pending = (XrXirValue){0};
    return array;
}
XRAY_API XrtArray *xrt_array_i64_new(const int64_t *values, size_t count, uint32_t source_id) {
    return array_new(values, count, XRT_ARRAY_I64, source_id);
}
XRAY_API XrtArray *xrt_array_string_new(XrtString *const *values, size_t count, uint32_t source_id) {
    return array_new(values, count, XRT_ARRAY_STRING, source_id);
}
static XrtArray *array_repeat(int64_t length, XrXirValue fill, XrtArrayElement element, uint32_t source_id) {
    XrtValueRoot *root = xrt_value_root_current(source_id);
    size_t stride = element == XRT_ARRAY_I64 ? sizeof(int64_t) : sizeof(void *);
    if (length >= 0 && (uint64_t)length > SIZE_MAX / stride) xrt_value_failure(XR_XIR_VALUE_LIMIT, source_id);
    XR_CHECK(!root->pending.type && !root->pending.payload, "Array repeat starts with an empty pending owner");
    XrXirValueAdmission admission = array_admission(root, source_id);
    array_reserve(root, source_id);
    XrXirValueStatus status = xr_xir_array_repeat(xrt_array_type(element), length, &fill, &admission, &root->pending);
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source_id);
    XrtArray *array = (XrtArray *)(intptr_t)root->pending.payload;
    array_adopt(root, root->pending, element);
    root->pending = (XrXirValue){0};
    return array;
}
XRAY_API XrtArray *xrt_array_i64_repeat(int64_t length, int64_t fill, uint32_t source_id) {
    return array_repeat(length, (XrXirValue){XR_XIR_I64, 0, fill}, XRT_ARRAY_I64, source_id);
}
XRAY_API XrtArray *xrt_array_string_repeat(int64_t length, XrtString *fill, uint32_t source_id) {
    return array_repeat(length, (XrXirValue){XR_XIR_STRING, 0, (int64_t)(intptr_t)fill}, XRT_ARRAY_STRING, source_id);
}
XR_FUNC XrXirValueStatus xrt_array_owned_copy(const XrtArray *array, const XrXirTypeArena *types,
    XrtArrayElement element, XrtArray **output) {
    if (!array || !types || !output || *output || (unsigned)element > XRT_ARRAY_STRING)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValue value = array_value(array), copy = {0};
    if (!array_shape(&value, types, element)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueAdmission admission = {xr_xir_value_arena(&value), ((const XirObject *)array)->domain,
        NULL, NULL, UINT64_MAX, UINT64_MAX};
    XrXirValueStatus status = xr_xir_value_admit(&value, (XrXirType)value.type, &admission);
    if (status == XR_XIR_VALUE_OK) status = xr_xir_value_copy(&value, &copy);
    if (status == XR_XIR_VALUE_OK) *output = (XrtArray *)(intptr_t)copy.payload;
    return status;
}
XRAY_API void xrt_array_owned_drop(XrtArray **array) {
    if (!array || !*array) return;
    XrXirValue value = array_value(*array);
    *array = NULL; xr_xir_value_drop(&value);
}
XR_FUNC XrtArray *xrt_array_root_take(XrtArray *array) {
    XrtValueRoot *root = xrt_value_root_current(0);
    XrtArrayOwner *owner = array_find(root, (int64_t)(intptr_t)array);
    XR_CHECK(owner && owner->count, "Array result transfer requires an authenticated current guest owner");
    --root->array_held;
    if (!--owner->count) array_remove(root, owner);
    return array;
}
XRAY_API XrtArray *xrt_array_retain(const XrtArray *array, uint32_t source_id) {
    XrtValueRoot *root = xrt_value_root_current(source_id);
    XrtArrayElement element = XRT_ARRAY_I64;
    if (!array_current_element(root, array, &element)) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source_id);
    XrXirValueAdmission admission = array_admission(root, source_id);
    XrXirValue value = array_argument(root, array, &admission, element, source_id);
    array_reserve(root, source_id);
    XrXirValue copy = {0};
    XrXirValueStatus status = xr_xir_value_copy(&value, &copy);
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source_id);
    array_adopt(root, copy, element);
    return (XrtArray *)(intptr_t)copy.payload;
}
XRAY_API void xrt_array_drop(XrtArray **array) {
    if (!array || !*array) return;
    XrtValueRoot *root = xrt_value_root_current(0);
    XrtArrayOwner *owner = array_find(root, (int64_t)(intptr_t)*array);
    XR_CHECK(owner && owner->count, "Array drop consumes one actual guest owner");
    XrXirValue value = owner->value;
    *array = NULL;
    --root->array_held;
    if (!--owner->count) array_remove(root, owner);
    xr_xir_value_drop(&value);
}
XRAY_API void xrt_array_assign(XrtArray **destination, const XrtArray *source, uint32_t source_id) {
    if (!destination) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source_id);
    if (*destination) {
        XrXirValue old = array_value(*destination), next = array_value(source);
        XrtArrayElement before = XRT_ARRAY_I64, after = XRT_ARRAY_I64;
        if (!array_element(&old, &before) || !array_element(&next, &after) || before != after)
            xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source_id);
    }
    XrtArray *copy = xrt_array_retain(source, source_id);
    xrt_array_drop(destination); *destination = copy;
}
XRAY_API int64_t xrt_array_length(const XrtArray *array, uint32_t source_id) {
    XrtValueRoot *root = xrt_value_root_current(source_id);
    XrtArrayElement element = XRT_ARRAY_I64;
    if (!array_current_element(root, array, &element)) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source_id);
    XrXirValueAdmission admission = array_admission(root, source_id);
    XrXirValue value = array_argument(root, array, &admission, element, source_id);
    int64_t length = 0;
    XrXirValueStatus status = xr_xir_array_len(&value, &admission, &length);
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source_id);
    return length;
}
static XrtValueRoot *array_get(const XrtArray *array, int64_t index, XrtArrayElement element, uint32_t source_id) {
    XrtValueRoot *root = xrt_value_root_current(source_id);
    XR_CHECK(!root->pending.type && !root->pending.payload, "Array read owns a distinct pending result");
    XrXirValueAdmission admission = array_admission(root, source_id);
    XrXirValue value = array_argument(root, array, &admission, element, source_id);
    XrXirFaultDetail fault = {0};
    XrXirValueStatus status = xr_xir_array_get(&value, index, &admission, &root->pending, &fault);
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source_id);
    return root;
}
XRAY_API int64_t xrt_array_i64_get(const XrtArray *array, int64_t index, uint32_t source_id) {
    XrtValueRoot *active = xrt_value_root_current(source_id);
    XrtArrayOwner *owner = array ? array_find(active, (int64_t)(intptr_t)array) : NULL;
    if (owner) {
        if (owner->element != XRT_ARRAY_I64) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source_id);
        XR_CHECK(!active->pending.type && !active->pending.payload, "Array read owns a distinct pending result");
        int64_t value = 0;
        XrXirValueStatus status = xr_xir_array_i64_read_owned(&owner->value, index, &value);
        if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source_id);
        return value;
    }
    XrtValueRoot *root = array_get(array, index, XRT_ARRAY_I64, source_id);
    int64_t value = root->pending.payload;
    xr_xir_value_drop(&root->pending);
    return value;
}
XRAY_API XrtString *xrt_array_string_get(const XrtArray *array, int64_t index, uint32_t source_id) {
    (void)array_get(array, index, XRT_ARRAY_STRING, source_id);
    return xrt_string_publish_pending(source_id);
}

static void array_mutate(XrtArray **binding, int64_t index, XrXirValue element, bool push, uint32_t source_id) {
    XrtValueRoot *root = xrt_value_root_current(source_id);
    if (!binding || !*binding) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source_id);
    XrXirValueAdmission admission = array_admission(root, source_id);
    XrtArrayElement kind = element.type == XR_XIR_I64 ? XRT_ARRAY_I64 : XRT_ARRAY_STRING;
    XrXirValue value = array_argument(root, *binding, &admission, kind, source_id);
    int64_t old = value.payload;
    XrtArrayOwner *owner = array_find(root, old);
    XR_CHECK(owner && owner->count, "Array mutation binds an owned current activation slot");
    XrXirValuePlace place = {(XrXirType)value.type, &value.payload};
    XrXirFaultDetail fault = {0};
    XrXirValueStatus status = push && kind == XRT_ARRAY_I64 ?
        xr_xir_array_i64_push_owned(&value, element.payload, admission.domain) :
        push ? xr_xir_array_push(&place, &element, &admission) :
            xr_xir_array_set(&place, index, &element, &admission, &fault);
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source_id);
    if (value.payload != old) {
        owner = array_find(root, old);
        --root->array_held;
        if (!--owner->count) array_remove(root, owner);
        /* The canonical place already transferred this one real reference. */
        value.type = (uint32_t)((const XirObject *)(intptr_t)value.payload)->type;
        array_adopt(root, value, kind);
        *binding = (XrtArray *)(intptr_t)value.payload;
    }
}
XRAY_API void xrt_array_i64_set(XrtArray **array, int64_t index, int64_t value, uint32_t source_id) {
    array_mutate(array, index, (XrXirValue){XR_XIR_I64, 0, value}, false, source_id);
}
XRAY_API void xrt_array_string_set(XrtArray **array, int64_t index, XrtString *value, uint32_t source_id) {
    array_mutate(array, index, (XrXirValue){XR_XIR_STRING, 0, (int64_t)(intptr_t)value}, false, source_id);
}
XRAY_API void xrt_array_i64_push(XrtArray **array, int64_t value, uint32_t source_id) {
    array_mutate(array, 0, (XrXirValue){XR_XIR_I64, 0, value}, true, source_id);
}
XRAY_API void xrt_array_string_push(XrtArray **array, XrtString *value, uint32_t source_id) {
    array_mutate(array, 0, (XrXirValue){XR_XIR_STRING, 0, (int64_t)(intptr_t)value}, true, source_id);
}
XR_FUNC void xrt_array_root_drain(XrtValueRoot *root) {
    for (uint32_t i = 0; i < root->array_capacity; ++i) {
        XrtArrayOwner owner = root->arrays[i];
        for (uint32_t n = 0; n < owner.count; ++n) {
            XrXirValue value = owner.value;
            xr_xir_value_drop(&value);
        }
    }
    if (root->arrays) xr_xir_domain_deallocate(root->domain, root->arrays,
        (size_t)root->array_capacity * sizeof(*root->arrays));
    root->arrays = NULL; root->array_capacity = root->array_count = 0; root->array_held = 0;
    xr_xir_compile_type_arena_drop(root->types); root->types = NULL;
}
