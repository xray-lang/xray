/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_string_backing.c - Complete UTF-8 construction in stable pending slots
 *
 * KEY CONCEPT:
 *   Replacement is published only after its checked bytes and rune count exist.
 */
#include "xnative_string_backing.h"
#include "base/xchecks.h"
#include "shared/xr_utf8_core.h"
#include <string.h>

static XrtManagedStatus string_extent(size_t length, size_t reserve, size_t *bytes) {
    if (length == SIZE_MAX) return XRT_MANAGED_OVERFLOW;
    size_t capacity = length + 1;
    if (reserve > capacity) capacity = reserve;
    if (capacity > SIZE_MAX - sizeof(XrtStringBacking)) return XRT_MANAGED_OVERFLOW;
    *bytes = sizeof(XrtStringBacking) + capacity;
    return XRT_MANAGED_OK;
}
static XrtManagedStatus string_finish(XrtManagedFrame *frame, size_t slot) {
    XrtManagedStatus status = xrt_managed_slot_commit(frame, slot);
    if (status != XRT_MANAGED_OK)
        XR_CHECK(xrt_managed_slot_abort(frame, slot) == XRT_MANAGED_OK, "factory aborts only its prepared token");
    return status;
}
XR_FUNCDEF XrtManagedStatus xrt_string_backing_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, const char *bytes, size_t length, size_t reserve) {
    if (!types || !types->string || (length && !bytes)) return XRT_MANAGED_ARGUMENT;
    size_t extent = 0;
    XrtManagedStatus status = string_extent(length, reserve, &extent);
    if (status != XRT_MANAGED_OK) return status;
    XrUtf8ScanResult scan = xr_utf8_core_scan_strict((const uint8_t *)bytes, length);
    if (scan.error != XR_UTF8_OK) return XRT_MANAGED_BAD_UTF8;
    XrtManagedHeader *payload = NULL;
    status = xrt_managed_slot_prepare(frame, slot, types->string, extent, &payload);
    if (status != XRT_MANAGED_OK) return status;
    XrtStringBacking *string = (XrtStringBacking *)payload;
    XR_CHECK(string->capacity > length, "the kernel derives the complete String extent");
    if (length) memmove(string->bytes, bytes, length);
    string->bytes[length] = 0;
    string->length = length;
    string->runes = scan.rune_count;
    return string_finish(frame, slot);
}
XR_FUNCDEF XrtManagedStatus xrt_string_backing_concat_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, const XrtStringBacking *left, const XrtStringBacking *right,
    size_t reserve) {
    if (!types || !types->string || !left || !right) return XRT_MANAGED_ARGUMENT;
    if (right->length > SIZE_MAX - left->length || right->runes > SIZE_MAX - left->runes)
        return XRT_MANAGED_OVERFLOW;
    size_t extent = 0, length = left->length + right->length;
    XrtManagedStatus status = string_extent(length, reserve, &extent);
    if (status != XRT_MANAGED_OK) return status;
    XrtManagedHeader *payload = NULL;
    status = xrt_managed_slot_prepare(frame, slot, types->string, extent, &payload);
    if (status != XRT_MANAGED_OK) return status;
    XrtStringBacking *string = (XrtStringBacking *)payload;
    XR_CHECK(string->capacity > length, "concat pending storage covers both borrowed inputs");
    if (left->length) memcpy(string->bytes, left->bytes, left->length);
    if (right->length) memcpy(string->bytes + left->length, right->bytes, right->length);
    string->bytes[length] = 0;
    string->length = length;
    string->runes = left->runes + right->runes;
    return string_finish(frame, slot);
}
