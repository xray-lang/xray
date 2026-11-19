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
#include "shared/xr_float_to_decimal.h"
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
    XrtStringBacking *string = NULL;
    status = xrt_string_backing_prepare(frame, slot, types->string, extent, &string);
    if (status != XRT_MANAGED_OK) return status;
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
    XrtStringBacking *string = NULL;
    status = xrt_string_backing_prepare(frame, slot, types->string, extent, &string);
    if (status != XRT_MANAGED_OK) return status;
    XR_CHECK(string->capacity > length, "concat pending storage covers both borrowed inputs");
    if (left->length) memcpy(string->bytes, left->bytes, left->length);
    if (right->length) memcpy(string->bytes + left->length, right->bytes, right->length);
    string->bytes[length] = 0;
    string->length = length;
    string->runes = left->runes + right->runes;
    return string_finish(frame, slot);
}
typedef struct StringFormatBuffer {
    char *bytes;
    size_t length, capacity;
    bool overflow;
} StringFormatBuffer;

static int string_format_write(void *context, const void *bytes, size_t count) {
    StringFormatBuffer *buffer = context;
    if (count > SIZE_MAX - buffer->length) {
        buffer->overflow = true;
        return 0;
    }
    if (buffer->bytes) {
        if (count > buffer->capacity - buffer->length) return 0;
        if (count) memcpy(buffer->bytes + buffer->length, bytes, count);
    }
    buffer->length += count;
    return 1;
}

XR_FUNCDEF XrtManagedStatus xrt_string_backing_format_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, XrValueFormatReader reader, XrValueFormatNode root) {
    if (!types || !types->string) return XRT_MANAGED_ARGUMENT;
    StringFormatBuffer buffer = {0};
    XrValueFormatSink sink = {&buffer, string_format_write};
    if (!xr_value_format_value(reader, root, sink, 0))
        return buffer.overflow ? XRT_MANAGED_OVERFLOW : XRT_MANAGED_ARGUMENT;
    size_t length = buffer.length, extent = 0;
    XrtManagedStatus status = string_extent(length, 0, &extent);
    if (status != XRT_MANAGED_OK) return status;
    XrtStringBacking *string = NULL;
    status = xrt_string_backing_prepare(frame, slot, types->string, extent, &string);
    if (status != XRT_MANAGED_OK) return status;
    XR_CHECK(string->capacity > length, "format pending storage covers the measured text");
    buffer = (StringFormatBuffer){.bytes = string->bytes, .capacity = length};
    if (!xr_value_format_value(reader, root, sink, 0) || buffer.length != length) {
        XR_CHECK(xrt_managed_slot_abort(frame, slot) == XRT_MANAGED_OK,
            "a failed formatter retires only its pending String");
        return buffer.overflow ? XRT_MANAGED_OVERFLOW : XRT_MANAGED_ARGUMENT;
    }
    XrUtf8ScanResult scan = xr_utf8_core_scan_strict((const uint8_t *)string->bytes, length);
    XR_CHECK(scan.error == XR_UTF8_OK, "shared formatting preserves admitted String UTF-8");
    string->bytes[length] = 0;
    string->length = length;
    string->runes = scan.rune_count;
    return string_finish(frame, slot);
}

static const XrtValueImageType *tuple_format_unwrap(const XrtValueImage *image, XrValueFormatNode *node) {
    const XrtValueImageType *type = &image->types[node->type - 1];
    while (type->kind == XRT_VALUE_NULLABLE) {
        const unsigned char *bytes = node->value;
        uint8_t tag = bytes[type->nullable_tag];
        if (tag > 1) return NULL;
        if (!tag) {
            node->value = NULL;
            break;
        }
        node->value = bytes + type->nullable_payload;
        node->type = type->nullable_element;
        type = &image->types[node->type - 1];
    }
    return type;
}

static int tuple_format_read(const void *context, XrValueFormatNode node, XrValueFormatView *view) {
    const XrtValueImageType *type = tuple_format_unwrap(context, &node);
    if (!type) return 0;
    if (!node.value) {
        view->kind = XR_VALUE_FORMAT_LITERAL;
        view->name = "null";
        view->name_size = 4;
        return 1;
    }
    switch (type->kind) {
        case XRT_VALUE_TUPLE:
            view->kind = XR_VALUE_FORMAT_TUPLE;
            view->unsigned_value = type->field_count;
            return 1;
        case XRT_VALUE_I64:
            view->kind = XR_VALUE_FORMAT_SIGNED;
            memcpy(&view->signed_value, node.value, sizeof(view->signed_value));
            return 1;
        case XRT_VALUE_BOOL:
            view->kind = XR_VALUE_FORMAT_BOOL;
            view->unsigned_value = *(const uint8_t *)node.value;
            return view->unsigned_value <= 1;
        case XRT_VALUE_F64: {
            uint64_t bits = 0;
            memcpy(&bits, node.value, sizeof(bits));
            view->kind = XR_VALUE_FORMAT_BYTES;
            view->bytes = view->scalar_bytes;
            return xr_float_to_decimal(64, bits, (char *)view->scalar_bytes, sizeof(view->scalar_bytes), &view->size);
        }
        case XRT_VALUE_STRING: {
            const XrtStringBacking *string = NULL;
            memcpy(&string, node.value, sizeof(string));
            if (!string) return 0;
            view->kind = XR_VALUE_FORMAT_BYTES;
            view->bytes = (const unsigned char *)string->bytes;
            view->size = string->length;
            view->quote = '"';
            return 1;
        }
        default:
            return 0;
    }
}

static int tuple_format_child(const void *context, XrValueFormatNode node, uint32_t index,
    XrValueFormatNode *child) {
    const XrtValueImage *image = context;
    const XrtValueImageType *type = tuple_format_unwrap(image, &node);
    if (!type || !node.value || type->kind != XRT_VALUE_TUPLE || index >= type->field_count) return 0;
    const XrtValueImageSlot *field = &image->slots[type->first_field + index];
    *child = (XrValueFormatNode){(const unsigned char *)node.value + field->offset, field->type};
    return 1;
}

XR_FUNCDEF XrtManagedStatus xrt_string_backing_tuple_display_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, const XrtValueImage *image, uint32_t type_id, const void *bytes) {
    if (!bytes || !image || !image->types || !type_id || type_id > image->type_count ||
        image->types[type_id - 1].kind != XRT_VALUE_TUPLE)
        return XRT_MANAGED_ARGUMENT;
    XrValueFormatReader reader = {image, tuple_format_read, tuple_format_child};
    return xrt_string_backing_format_create(frame, slot, types, reader, (XrValueFormatNode){bytes, type_id});
}
