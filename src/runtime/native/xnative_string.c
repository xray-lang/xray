/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_string.c - Typed access to the shared String and RC kernel
 *
 * KEY CONCEPT:
 *   Guest locals are lost when a failure ends the root, so the root itself
 *   records every String its guest owns. It holds one real reference per
 *   String and counts guest references in an open-addressed table: retain and
 *   drop touch no shared state, and ending the root releases each real
 *   reference once. Root Strings are single unpublished blocks in the root
 *   domain, whose live-byte limit charges every body and table. A literal is
 *   one pinned object per root, lent to the guest without a count.
 */
#include "xnative_string.h"
#include "xnative_array.h"
#include "xir/xxir_value_internal.h"
#include "xir/xxir_float.h"
#include "base/xchecks.h"
#include <string.h>

/* count is the number of guest references; a pinned literal slot keeps its
 * real reference at count zero until the root ends. */
struct XrtStringOwner {
    XrtString *string;
    uint32_t count;
    bool pinned;
};
struct XrtStringLiteral {
    const char *bytes;
    size_t length;
    XrtString *string;
};
enum { ROOT_TABLE_MINIMUM = 16 };
_Static_assert(ROOT_TABLE_MINIMUM > 0 && (ROOT_TABLE_MINIMUM & (ROOT_TABLE_MINIMUM - 1)) == 0,
    "root tables use power-of-two capacities");

static XR_THREAD_LOCAL XrtValueBinding string_binding;
XR_FUNC XrtValueBinding xrt_value_root_bind(XrtValueBinding binding) {
    XrtValueBinding previous = string_binding;
    string_binding = binding;
    return previous;
}
XR_FUNC XrtValueRoot *xrt_value_root_active(void) { return string_binding.root; }
XR_FUNC XrtValueRoot *xrt_value_root_current(uint32_t source_id) {
    const XrtValueBinding *binding = &string_binding;
    XR_CHECK(binding->root && source_id <= binding->sources, "Value operations require an active root and source id");
    return binding->root;
}
static XrtValueRoot *string_root(uint32_t source_id) { return xrt_value_root_current(source_id); }
static size_t root_slot(const void *key, uint32_t capacity) {
    uint64_t hash = (uint64_t)(uintptr_t)key * UINT64_C(0x9e3779b97f4a7c15);
    return (size_t)(hash >> 32) & (capacity - 1);
}
static XrXirValue string_value(const XrtString *string) {
    XrXirValue value = {XR_XIR_STRING, 0, (int64_t)(intptr_t)string};
    return value;
}
static void string_check(const XrtString *string) {
    XrXirValue value = string_value(string);
    XR_CHECK(string && xr_xir_value_valid(&value), "A native String borrows a canonical live object");
}
static XrtStringOwner *owner_find(const XrtValueRoot *root, const XrtString *string) {
    if (!root->table) return NULL;
    uint32_t mask = root->table_capacity - 1;
    for (size_t i = root_slot(string, root->table_capacity);; i = (i + 1) & mask) {
        XrtStringOwner *slot = &root->table[i];
        if (slot->string == string) return slot;
        if (!slot->string) return NULL;
    }
}
/* A held slot proves a live canonical String; any other handle is validated. */
static void root_borrow(const XrtValueRoot *root, const XrtString *string) {
    if (!string || !root || !owner_find(root, string)) string_check(string);
}
XR_FUNC XrXirDomain *xrt_value_root_domain(XrtValueRoot *root, uint32_t source_id) {
    if (!root->domain) {
        XrXirValueStatus status = xr_xir_root_domain_new(root->byte_limit ? root->byte_limit : UINT64_MAX,
            &root->domain);
        if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source_id);
    }
    return root->domain;
}
static XrXirDomain *root_domain(XrtValueRoot *root, uint32_t source_id) {
    return xrt_value_root_domain(root, source_id);
}
static uint32_t root_grown(uint32_t capacity, uint32_t source_id) {
    if (capacity > UINT32_MAX / 4) xrt_value_failure(XR_XIR_VALUE_LIMIT, source_id);
    return capacity ? capacity * 2 : ROOT_TABLE_MINIMUM;
}
static void owner_place(XrtStringOwner *table, uint32_t capacity, XrtStringOwner owner) {
    for (size_t i = root_slot(owner.string, capacity);; i = (i + 1) & (capacity - 1))
        if (!table[i].string) { table[i] = owner; return; }
}
/* Make room for one more owner before anything is created, so publication
 * cannot fail after a real reference exists. Load stays at most one half. */
static void root_reserve(XrtValueRoot *root, uint32_t source_id) {
    XrXirDomain *domain = root_domain(root, source_id);
    if (root->table_count < root->table_capacity / 2) return;
    uint32_t capacity = root_grown(root->table_capacity, source_id);
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    XrtStringOwner *table = xr_xir_domain_allocate(domain, (size_t)capacity * sizeof(*table), &status);
    if (!table) xrt_value_failure(status, source_id);
    memset(table, 0, (size_t)capacity * sizeof(*table));
    for (uint32_t i = 0; i < root->table_capacity; ++i)
        if (root->table[i].string) owner_place(table, capacity, root->table[i]);
    if (root->table) xr_xir_domain_deallocate(domain, root->table, (size_t)root->table_capacity * sizeof(*table));
    root->table = table;
    root->table_capacity = capacity;
}
/* The handle is absent: a new object cannot alias a live one, and foreign
 * handles are inserted only after a failed lookup. */
static XrtString *root_adopt(XrtValueRoot *root, XrtString *string, bool pinned) {
    owner_place(root->table, root->table_capacity, (XrtStringOwner){string, 1, pinned});
    ++root->table_count;
    ++root->held;
    return string;
}
static XrtString *root_count(XrtValueRoot *root, XrtStringOwner *owner, uint32_t source_id) {
    if (owner->count == UINT32_MAX) xrt_value_failure(XR_XIR_VALUE_REFCOUNT_LIMIT, source_id);
    ++owner->count;
    ++root->held;
    return owner->string;
}
/* Backward-shift deletion keeps every probe sequence free of holes. */
static void owner_remove(XrtValueRoot *root, XrtStringOwner *slot) {
    uint32_t mask = root->table_capacity - 1;
    size_t hole = (size_t)(slot - root->table);
    for (size_t next = (hole + 1) & mask; root->table[next].string; next = (next + 1) & mask) {
        size_t home = root_slot(root->table[next].string, root->table_capacity);
        if (((next - home) & mask) >= ((next - hole) & mask)) {
            root->table[hole] = root->table[next];
            hole = next;
        }
    }
    root->table[hole] = (XrtStringOwner){0};
    --root->table_count;
}
static void root_release(XrtValueRoot *root, XrtStringOwner *owner) {
    --owner->count;
    --root->held;
    if (owner->count || owner->pinned) return;
    XrtString *string = owner->string;
    owner_remove(root, owner);
    xr_xir_string_release(string);
}
static XrtString *root_publish(XrtValueRoot *root, XrtString *string, XrXirValueStatus status,
    uint32_t source_id) {
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source_id);
    return root_adopt(root, string, false);
}
static XrtStringLiteral *literal_find(const XrtValueRoot *root, const char *bytes, size_t length) {
    if (!root->literals) return NULL;
    uint32_t mask = root->literal_capacity - 1;
    for (size_t i = root_slot(bytes, root->literal_capacity);; i = (i + 1) & mask) {
        XrtStringLiteral *slot = &root->literals[i];
        if (!slot->string) return NULL;
        if (slot->bytes == bytes && slot->length == length) return slot;
    }
}
static void literal_place(XrtStringLiteral *table, uint32_t capacity, XrtStringLiteral literal) {
    for (size_t i = root_slot(literal.bytes, capacity);; i = (i + 1) & (capacity - 1))
        if (!table[i].string) { table[i] = literal; return; }
}
static void literal_reserve(XrtValueRoot *root, uint32_t source_id) {
    if (root->literal_count < root->literal_capacity / 2) return;
    uint32_t capacity = root_grown(root->literal_capacity, source_id);
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    XrtStringLiteral *table = xr_xir_domain_allocate(root->domain, (size_t)capacity * sizeof(*table), &status);
    if (!table) xrt_value_failure(status, source_id);
    memset(table, 0, (size_t)capacity * sizeof(*table));
    for (uint32_t i = 0; i < root->literal_capacity; ++i)
        if (root->literals[i].string) literal_place(table, capacity, root->literals[i]);
    if (root->literals)
        xr_xir_domain_deallocate(root->domain, root->literals, (size_t)root->literal_capacity * sizeof(*table));
    root->literals = table;
    root->literal_capacity = capacity;
}
XRAY_API XrtString *xrt_string_new(const char *bytes, size_t length, uint32_t source_id) {
    XrtValueRoot *root = string_root(source_id);
    root_reserve(root, source_id);
    XrtString *string = NULL;
    XrXirValueStatus status = xr_xir_root_string_new(root->domain, bytes, length, &string);
    return root_publish(root, string, status, source_id);
}
/* The pinned slot holds the root's real reference and no guest owner, so a
 * drop of the borrowed handle fails closed like any other non-owner. */
XRAY_API XrtString *xrt_string_literal(const char *bytes, size_t length, uint32_t source_id) {
    XrtValueRoot *root = string_root(source_id);
    XrtStringLiteral *literal = literal_find(root, bytes, length);
    if (literal) return literal->string;
    root_reserve(root, source_id);
    literal_reserve(root, source_id);
    XrtString *string = NULL;
    XrXirValueStatus status = xr_xir_root_string_new(root->domain, bytes, length, &string);
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source_id);
    literal_place(root->literals, root->literal_capacity, (XrtStringLiteral){bytes, length, string});
    ++root->literal_count;
    owner_place(root->table, root->table_capacity, (XrtStringOwner){string, 0, true});
    ++root->table_count;
    return string;
}
XRAY_API XrtString *xrt_string_i64(int64_t value, uint32_t source_id) {
    XrtValueRoot *root = string_root(source_id);
    root_reserve(root, source_id);
    XrtString *string = NULL;
    XrXirValueStatus status = xr_xir_root_string_i64(root->domain, value, &string);
    return root_publish(root, string, status, source_id);
}
XRAY_API XrtString *xrt_string_f64(uint64_t bits, uint32_t source_id) {
    char bytes[32];
    size_t length = 0;
    XR_CHECK(xr_xir_float_format(64, bits, bytes, sizeof(bytes), &length),
        "The canonical binary64 formatter accepts every object bit pattern");
    return xrt_string_new(bytes, length, source_id);
}
XRAY_API XrtString *xrt_string_concat(const XrtString *left, const XrtString *right,
    uint32_t source_id) {
    XrtValueRoot *root = string_root(source_id);
    root_borrow(root, left);
    root_borrow(root, right);
    root_reserve(root, source_id);
    XrtString *string = NULL;
    XrXirValueStatus status = xr_xir_root_string_concat(root->domain, left, right, &string);
    return root_publish(root, string, status, source_id);
}
XRAY_API XrtString *xrt_string_retain(const XrtString *string, uint32_t source_id) {
    XrtValueRoot *root = string_root(source_id);
    XrtStringOwner *owner = string ? owner_find(root, string) : NULL;
    if (owner) return root_count(root, owner, source_id);
    string_check(string);
    root_reserve(root, source_id);
    XrXirValue source = string_value(string), output = {0};
    XrXirValueStatus status = xr_xir_value_copy(&source, &output);
    return root_publish(root, (XrtString *)(intptr_t)output.payload, status, source_id);
}
XR_FUNC XrXirValueStatus xrt_string_owned_copy(const XrtString *string, XrtString **out) {
    if (!string || !out || *out) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValue source = string_value(string), value = {0};
    XrXirValueStatus status = xr_xir_value_copy(&source, &value);
    if (status == XR_XIR_VALUE_OK) *out = (XrtString *)(intptr_t)value.payload;
    return status;
}
XR_FUNC XrtString *xrt_string_root_take(XrtString *string) {
    XrtValueRoot *root = string_root(0);
    XrtStringOwner *owner = string ? owner_find(root, string) : NULL;
    XR_CHECK(owner && owner->count, "Only an actual current root owner can be transferred");
    if (owner->count == 1 && !owner->pinned) {
        XrXirValueStatus escape = string->object.root_private ? xr_xir_root_string_escape(string) : XR_XIR_VALUE_OK;
        if (escape != XR_XIR_VALUE_OK) xrt_value_failure(escape, 0);
        owner_remove(root, owner); // The root's real reference moves to the receiver.
        --root->held;
        return string;
    }
    XrXirValue source = string_value(string), output = {0};
    XrXirValueStatus status = xr_xir_value_copy(&source, &output);
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, 0);
    --owner->count;
    --root->held;
    return string;
}
XRAY_API void xrt_string_move(XrtString **destination, XrtString **source) {
    XrtValueRoot *root = string_root(0);
    XrtStringOwner *owner = source && *source ? owner_find(root, *source) : NULL;
    XR_CHECK(destination && source && destination != source && !*destination && owner && owner->count,
        "Move consumes one current String owner into an empty slot");
    *destination = *source;
    *source = NULL;
}
XRAY_API void xrt_string_drop(XrtString **string) {
    if (!string || !*string) return;
    XrtValueRoot *root = string_root(0);
    XrtStringOwner *owner = owner_find(root, *string);
    XR_CHECK(owner && owner->count, "Only an actual current root owner can be dropped");
    *string = NULL;
    root_release(root, owner);
}
XRAY_API void xrt_string_owned_drop(XrtString **string) {
    if (!string || !*string) return;
    XrtString *owned = *string;
    string_check(owned);
    *string = NULL;
    xr_xir_string_release(owned);
}
XRAY_API const char *xrt_string_bytes(const XrtString *string, size_t *length) {
    root_borrow(string_binding.root, string);
    XR_CHECK(length, "String bytes require a live canonical String");
    *length = string->length;
    return string->bytes;
}
XRAY_API int64_t xrt_string_length(const XrtString *string, uint32_t source_id) {
    root_borrow(string_binding.root, string);
    if (string->runes > INT64_MAX) xrt_value_failure(XR_XIR_VALUE_LIMIT, source_id);
    return (int64_t)string->runes;
}
XR_FUNC void xrt_value_root_drain(XrtValueRoot *root) {
    xr_xir_value_drop(&root->pending);
    xrt_array_root_drain(root);
    for (uint32_t i = 0; i < root->table_capacity; ++i)
        if (root->table[i].string) xr_xir_string_release(root->table[i].string);
    if (root->table)
        xr_xir_domain_deallocate(root->domain, root->table, (size_t)root->table_capacity * sizeof(*root->table));
    if (root->literals)
        xr_xir_domain_deallocate(root->domain, root->literals,
            (size_t)root->literal_capacity * sizeof(*root->literals));
    XrXirDomain *domain = root->domain;
    *root = (XrtValueRoot){.byte_limit = root->byte_limit};
    xr_xir_root_domain_release(domain);
}

XR_FUNC XrtString *xrt_string_publish_pending(uint32_t source_id) {
    XrtValueRoot *root = string_root(source_id);
    XR_CHECK(root->pending.type == XR_XIR_STRING && xr_xir_value_valid(&root->pending),
        "String publication consumes the active root's owned pending result");
    XrtString *string = (XrtString *)(intptr_t)root->pending.payload;
    XrtStringOwner *owner = owner_find(root, string);
    if (owner) {
        root_count(root, owner, source_id);
        xr_xir_value_drop(&root->pending);
    } else {
        root_reserve(root, source_id);
        root_adopt(root, string, false);
        root->pending = (XrXirValue){0};
    }
    return string;
}
