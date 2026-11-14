/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_boxed.c - Physical owners of canonical record and nullable snapshots
 *
 * KEY CONCEPT:
 *   The value kernel alone constructs and copies payloads. Root bookkeeping
 *   owns real references and remains reachable through every recovery jump.
 */
#include "xnative_boxed.h"
#include "xnative_string.h"
#include "xnative_callable.h"
#include "xnative_module_state.h"
#include "xir/xxir_struct.h"
#include "xir/xxir_nullable.h"
#include "xir/xxir_types.h"
#include "xir/xxir_value_internal.h"
#include "base/xchecks.h"
#include <string.h>

typedef struct XrtBoxedOwner {
    struct XrtBoxedOwner *next;
    XrXirValue value;
    uint32_t count;
} XrtBoxedOwner;
typedef struct XrtBoxedCatalog {
    struct XrtBoxedCatalog *next;
    const XrtBoxedImage *image;
    XrXirTypeArena *types;
} XrtBoxedCatalog;

static XrXirValue boxed_value(const XrtBoxed *box) {
    const XirObject *object = (const XirObject *)box;
    return object ? (XrXirValue){(uint32_t)object->type, 0, (int64_t)(intptr_t)box} : (XrXirValue){0};
}
static void boxed_require(XrXirValueStatus status, uint32_t source) {
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
}
static XrtBoxedOwner **boxed_owner(XrtValueRoot *root, const XrtBoxed *box) {
    XrtBoxedOwner **cursor = &root->boxed;
    while (*cursor && (const void *)(intptr_t)(*cursor)->value.payload != box) cursor = &(*cursor)->next;
    return cursor;
}
static void *boxed_allocate(XrtValueRoot *root, size_t size, uint32_t source) {
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    void *storage = xr_xir_domain_allocate(xrt_value_root_domain(root, source), size, &status);
    boxed_require(status, source);
    return storage;
}
static XrtBoxed *boxed_publish(XrtValueRoot *root, uint32_t source) {
    XrtBoxed *box = (XrtBoxed *)(intptr_t)root->pending.payload;
    XrtBoxedOwner **entry = boxed_owner(root, box);
    if (*entry) {
        if ((*entry)->count == UINT32_MAX) xrt_value_failure(XR_XIR_VALUE_REFCOUNT_LIMIT, source);
        ++(*entry)->count;
    } else {
        XrtBoxedOwner *owned = boxed_allocate(root, sizeof(*owned), source);
        *owned = (XrtBoxedOwner){.value=root->pending, .count=1};
        *entry = owned;
    }
    root->pending = (XrXirValue){0};
    return box;
}
static XrXirTypeArena *boxed_catalog(const XrtBoxedImage *image, XrXirTypeArena *received, uint32_t source) {
    XrtValueRoot *root = xrt_value_root_current(source);
    if (!image || (image->count && !image->types)) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    for (XrtBoxedCatalog *catalog = root->boxed_catalogs; catalog; catalog = catalog->next)
        if (catalog->image == image) {
            if (received && received != catalog->types) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
            return catalog->types;
        }
    XrtBoxedCatalog *catalog = boxed_allocate(root, sizeof(*catalog), source);
    *catalog = (XrtBoxedCatalog){.next=root->boxed_catalogs, .image=image};
    root->boxed_catalogs = catalog;
    if (received) {
        if (!xr_xir_compile_type_arena_retain(received)) xrt_value_failure(XR_XIR_VALUE_REFCOUNT_LIMIT, source);
        catalog->types = received;
    } else boxed_require(xrt_callable_types_new(image->types, image->count,
        root->byte_limit ? root->byte_limit : UINT64_MAX, &catalog->types), source);
    return catalog->types;
}
XRAY_API void xrt_boxed_state_prepare(XrtModuleState *state, const XrtBoxedImage *image, uint32_t source) {
    if (!image) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    XrtCallableImage physical = {image->types, image->count, NULL, 0};
    XrXirTypeArena *types = xrt_module_state_callables(state, &physical);
    if (!types) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    (void)boxed_catalog(image, types, source);
}

XR_FUNC XrtBoxed *xrt_boxed_create(const XrXirTypeArena *arena, XrXirType type,
    const uint64_t *fields, uint32_t count, uint32_t source) {
    XrtValueRoot *root = xrt_value_root_current(source);
    const XrXirTypes *types = xr_xir_compile_type_arena_types(arena);
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    bool nullable = node && node->kind == XR_XIR_TYPE_NULLABLE;
    if (!node || (count && !fields) || (nullable ? count > 1 : !xr_xir_type_is_struct(types, type) ||
        node->nominal.field_count != count)) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    XR_CHECK(!root->pending.type, "one rooted pending value covers failure-atomic publication");
    XrXirValue *values = count ? boxed_allocate(root, (size_t)count * sizeof(*values), source) : NULL;
    /* This scratch is registered before any fallible value-kernel call. */
    root->boxed_scratch = values;
    root->boxed_scratch_size = (size_t)count * sizeof(*values);
    for (uint32_t f = 0; f < count; ++f) {
        XrXirType field = nullable ? node->element : node->nominal.fields[f];
        values[f] = (XrXirValue){(uint32_t)field, 0, 0};
        memcpy(&values[f].payload, &fields[f], sizeof(fields[f]));
    }
    XrXirValueAdmission admission = {arena, xrt_value_root_domain(root, source), NULL, NULL, UINT64_MAX, UINT64_MAX};
    XrXirValueStatus status = nullable ? xr_xir_nullable_new(type, count ? values : NULL, &admission, &root->pending) :
        xr_xir_struct_new(type, values, count, &admission, &root->pending);
    if (values) xr_xir_domain_deallocate(root->domain, values, root->boxed_scratch_size);
    root->boxed_scratch = NULL;
    root->boxed_scratch_size = 0;
    boxed_require(status, source);
    return boxed_publish(root, source);
}
XRAY_API XrtBoxed *xrt_boxed_new(const XrtBoxedImage *image, uint32_t type,
    const uint64_t *fields, uint32_t count, uint32_t source) {
    if (!image || type >= image->count) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    XrXirType exact = (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + XRT_CALLABLE_PREFIX_COUNT + type);
    return xrt_boxed_create(boxed_catalog(image, NULL, source), exact, fields, count, source);
}
XRAY_API XrtBoxed *xrt_boxed_retain(XrtBoxed *box, uint32_t source) {
    XrtValueRoot *root = xrt_value_root_current(source);
    XrXirValue value = boxed_value(box);
    XR_CHECK(!root->pending.type, "copy publishes into the rooted pending owner");
    boxed_require(xr_xir_value_copy(&value, &root->pending), source);
    return boxed_publish(root, source);
}
XRAY_API void xrt_boxed_drop(XrtBoxed *box, uint32_t source) {
    XrtValueRoot *root = xrt_value_root_current(source);
    XrtBoxedOwner **entry = boxed_owner(root, box), *owned = *entry;
    if (!owned || !owned->count) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    XrXirValue value = owned->value;
    if (!--owned->count) {
        *entry = owned->next;
        xr_xir_domain_deallocate(root->domain, owned, sizeof(*owned));
    }
    xr_xir_value_drop(&value);
}
XRAY_API bool xrt_boxed_present(const XrtBoxed *box, uint32_t source) {
    XrXirValue value = boxed_value(box);
    bool some = false;
    const XrXirValue *payload;
    if (!xr_xir_nullable_view(&value, &some, &payload)) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    return some;
}
XRAY_API uint64_t xrt_boxed_field(const XrtBoxed *box, uint32_t field, uint32_t source) {
    XrtValueRoot *root = xrt_value_root_current(source);
    XrXirValue value = boxed_value(box);
    const XrXirTypeArena *arena = xr_xir_value_arena(&value);
    const XrXirTypes *types = xr_xir_compile_type_arena_types(arena);
    XrXirValueStatus status;
    XR_CHECK(!root->pending.type, "field ownership is rooted before publication");
    if (xr_xir_type_is_nullable(types, (XrXirType)value.type)) {
        bool some;
        const XrXirValue *payload;
        if (field || !xr_xir_nullable_view(&value, &some, &payload)) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
        if (!some) xrt_null_unwrap(source);
        status = xr_xir_value_copy(payload, &root->pending);
    } else {
        XrXirValueAdmission admission = {arena, xrt_value_root_domain(root, source), NULL, NULL, UINT64_MAX, UINT64_MAX};
        status = xr_xir_struct_get(&value, field, &admission, &root->pending);
    }
    boxed_require(status, source);
    uint64_t bits;
    memcpy(&bits, &root->pending.payload, sizeof(bits));
    if (root->pending.type == XR_XIR_STRING) return (uint64_t)(uintptr_t)xrt_string_publish_pending(source);
    if (root->pending.type >= XR_XIR_CONSTRUCTED_TYPE_BASE) return (uint64_t)(uintptr_t)boxed_publish(root, source);
    root->pending = (XrXirValue){0};
    return bits;
}
XR_FUNC XrXirValueStatus xrt_boxed_owned_copy(const XrtBoxed *box, const XrXirTypeArena *arena,
    XrXirType type, XrtBoxed **empty) {
    if (!box || !empty || *empty) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValue value = boxed_value(box), received = {0};
    if (xr_xir_value_arena(&value) != arena || value.type != (uint32_t)type ||
        !xr_xir_value_argument(&value, arena, type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status = xr_xir_value_copy(&value, &received);
    if (status == XR_XIR_VALUE_OK) *empty = (XrtBoxed *)(intptr_t)received.payload;
    return status;
}
XR_FUNC void xrt_boxed_owned_drop(XrtBoxed **box) {
    if (!box || !*box) return;
    XrXirValue value = boxed_value(*box);
    *box = NULL;
    xr_xir_value_drop(&value);
}
XR_FUNC void xrt_boxed_root_drain(XrtValueRoot *root) {
    while (root->boxed) {
        XrtBoxedOwner *owner = root->boxed;
        root->boxed = owner->next;
        while (owner->count--) {
            XrXirValue value = owner->value;
            xr_xir_value_drop(&value);
        }
        xr_xir_domain_deallocate(root->domain, owner, sizeof(*owner));
    }
    while (root->boxed_catalogs) {
        XrtBoxedCatalog *catalog = root->boxed_catalogs;
        root->boxed_catalogs = catalog->next;
        xr_xir_compile_type_arena_drop(catalog->types);
        xr_xir_domain_deallocate(root->domain, catalog, sizeof(*catalog));
    }
    if (root->boxed_scratch) xr_xir_domain_deallocate(root->domain, root->boxed_scratch, root->boxed_scratch_size);
    root->boxed_scratch = NULL;
    root->boxed_scratch_size = 0;
}
