/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_format.c - Typed layout adapter for the shared value formatter
 *
 * KEY CONCEPT:
 *   Metadata and children borrow the original value owner. Floating text uses
 *   the same deterministic conversion as ordinary typed output.
 */
#include "xxir_format.h"
#include "xxir_format_internal.h"
#include "xxir_float.h"
#include "xxir_rune.h"

static int xir_format_read(const void *context, XrValueFormatNode node, XrValueFormatView *view) {
    XrXirValue borrowed = {0};
    if (!view || !xr_xir_value_format_storage(context, node, &borrowed, view)) return 0;
    if (view->kind) return 1;
    const XrXirValue *value = &borrowed;
    XrXirType type = (XrXirType)value->type;
    if (xr_xir_type_is_integer(type)) {
        view->kind = xr_xir_integer_signed(type) ? XR_VALUE_FORMAT_SIGNED : XR_VALUE_FORMAT_UNSIGNED;
        view->signed_value = value->payload;
        view->unsigned_value = (uint64_t)value->payload;
        return 1;
    }
    if (type == XR_XIR_BOOL) {
        view->kind = XR_VALUE_FORMAT_BOOL; view->unsigned_value = (uint64_t)value->payload;
        return 1;
    }
    if (type == XR_XIR_RUNE) {
        view->size=xr_xir_rune_utf8(value->payload,(char *)view->scalar_bytes);
        if (!view->size) return 0;
        view->kind=XR_VALUE_FORMAT_BYTES;view->bytes=view->scalar_bytes;view->quote=39; return 1;
    }
    if (type == XR_XIR_STRING) {
        const char *bytes = NULL;
        if (!xr_xir_string_view(value, &bytes, &view->size)) return 0;
        view->kind = XR_VALUE_FORMAT_BYTES; view->bytes = (const unsigned char *)bytes;
        view->quote = '"';
        return 1;
    }
    uint32_t floating = xr_xir_float_bits(type);
    if (floating) {
        if (!xr_xir_float_format(floating, (uint64_t)value->payload,
                (char *)view->scalar_bytes, sizeof(view->scalar_bytes), &view->size)) return 0;
        view->kind = XR_VALUE_FORMAT_BYTES; view->bytes = view->scalar_bytes; view->quote = '\0';
        return 1;
    }
    return 0;
}

static int xir_format_child(const void *context, XrValueFormatNode node, uint32_t index,
    XrValueFormatNode *child) {
    return xr_xir_value_format_child(context, node, index, child);
}

XR_FUNC XrValueFormatReader xr_xir_value_format_reader(const XrXirValue *root) {
    return (XrValueFormatReader){xr_xir_value_arena(root), xir_format_read, xir_format_child};
}
