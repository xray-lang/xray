/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_string.c - Typed access to the shared String and RC kernel
 */
#include "xnative_string.h"
#include "xir/xxir_value_internal.h"
#include "xir/xxir_output.h"
#include "xir/xxir_float.h"
#include "base/xchecks.h"

struct XrtStringOwner {
    XrtStringOwner *next;
    XrtString *string;
};

static XrXirValue string_borrow(const XrtString *string) {
    XrXirValue value = {XR_XIR_STRING, 0, (int64_t)(intptr_t)string};
    XR_CHECK(string && xr_xir_value_valid(&value), "A native String borrows a canonical live object");
    return value;
}
static XrtStringOwner *string_prepare(XrtStringRoot *root, uint32_t source_id) {
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    if (!root->domain) {
        status = xr_xir_domain_new(root->byte_limit ? root->byte_limit : UINT64_MAX, &root->domain);
        if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source_id);
    }
    XrtStringOwner *owner = xr_xir_domain_allocate(root->domain, sizeof(*owner), &status);
    if (!owner) xrt_value_failure(status, source_id);
    *owner = (XrtStringOwner){0};
    return owner;
}
static XrtString *string_publish(XrtStringRoot *root, XrtStringOwner *owner,
    XrXirValue *value, XrXirValueStatus status, uint32_t source_id) {
    if (status != XR_XIR_VALUE_OK) {
        xr_xir_value_drop(value);
        xr_xir_domain_deallocate(root->domain, owner, sizeof(*owner));
        xrt_value_failure(status, source_id);
    }
    XR_CHECK(value->type == XR_XIR_STRING && value->payload, "A String helper publishes one real owner");
    owner->string = (XrtString *)(intptr_t)value->payload;
    owner->next = root->owners;
    root->owners = owner;
    return owner->string;
}
XRAY_API XrtString *xrt_string_new(const char *bytes, size_t length, uint32_t source_id) {
    XrtStringRoot *root = xrt_string_root_current(source_id);
    XrtStringOwner *owner = string_prepare(root, source_id);
    XrXirValue value = {0};
    XrXirValueStatus status = xr_xir_string_new(root->domain, bytes, length, &value);
    return string_publish(root, owner, &value, status, source_id);
}
XRAY_API XrtString *xrt_string_i64(int64_t value, uint32_t source_id) {
    char bytes[32];
    XrXirValue scalar = {XR_XIR_I64, 0, value};
    const char *text = NULL;
    size_t length = 0;
    XR_CHECK(xr_xir_scalar_text(&scalar, bytes, &text, &length), "The canonical I64 formatter fits its fixed buffer");
    return xrt_string_new(text, length, source_id);
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
    XrtStringRoot *root = xrt_string_root_current(source_id);
    XrXirValue a = string_borrow(left), b = string_borrow(right), output = {0};
    XrtStringOwner *owner = string_prepare(root, source_id);
    XrXirValueStatus status = xr_xir_string_concat(root->domain, &a, &b, &output);
    return string_publish(root, owner, &output, status, source_id);
}
XRAY_API XrtString *xrt_string_retain(const XrtString *string, uint32_t source_id) {
    XrtStringRoot *root = xrt_string_root_current(source_id);
    XrXirValue source = string_borrow(string), output = {0};
    XrtStringOwner *owner = string_prepare(root, source_id);
    XrXirValueStatus status = xr_xir_value_copy(&source, &output);
    return string_publish(root, owner, &output, status, source_id);
}
static XrtStringOwner **string_owner(XrtStringRoot *root, const XrtString *string) {
    XrtStringOwner **link = &root->owners;
    while (*link && (*link)->string != string) link = &(*link)->next;
    return link;
}
XR_FUNC XrtString *xrt_string_root_take(XrtString *string) {
    XrtStringRoot *root = xrt_string_root_current(0);
    XrtStringOwner **link = string_owner(root, string);
    XR_CHECK(string && *link, "Only an actual current root owner can be transferred");
    XrtStringOwner *owner = *link;
    *link = owner->next;
    xr_xir_domain_deallocate(root->domain, owner, sizeof(*owner));
    return string;
}
XRAY_API void xrt_string_move(XrtString **destination, XrtString **source) {
    XrtStringRoot *root = xrt_string_root_current(0);
    XR_CHECK(destination && source && destination != source && !*destination &&
        *source && *string_owner(root, *source), "Move consumes one current String owner into an empty slot");
    *destination = *source;
    *source = NULL;
}
XRAY_API void xrt_string_drop(XrtString **string) {
    if (!string || !*string) return;
    XrtString *owned = xrt_string_root_take(*string);
    *string = NULL;
    xrt_string_owned_drop(&owned);
}
XRAY_API void xrt_string_owned_drop(XrtString **string) {
    if (!string || !*string) return;
    XrXirValue value = string_borrow(*string);
    *string = NULL;
    xr_xir_value_drop(&value);
}
XRAY_API const char *xrt_string_bytes(const XrtString *string, size_t *length) {
    XrXirValue value = string_borrow(string);
    const char *bytes = NULL;
    XR_CHECK(length && xr_xir_string_view(&value, &bytes, length), "String bytes require a live canonical String");
    return bytes;
}
XRAY_API int64_t xrt_string_length(const XrtString *string, uint32_t source_id) {
    XrXirValue value = string_borrow(string);
    int64_t length = 0;
    XrXirValueStatus status = xr_xir_string_length(&value, &length);
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source_id);
    return length;
}
XR_FUNC void xrt_string_root_drain(XrtStringRoot *root) {
    while (root->owners) {
        XrtStringOwner *owner = root->owners;
        root->owners = owner->next;
        XrXirValue value = string_borrow(owner->string);
        xr_xir_value_drop(&value);
        xr_xir_domain_deallocate(root->domain, owner, sizeof(*owner));
    }
    xr_xir_domain_drop(root->domain);
    root->domain = NULL;
}
