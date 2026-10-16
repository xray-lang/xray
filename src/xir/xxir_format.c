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
#include "xxir_enum.h"
#include "xxir_float.h"
#include "xxir_rune.h"

static bool xir_format_cursor(const void *context,XrValueFormatNode node,XrXirFormatCursor *output) {
    const XrXirFormatContext *format=context;
    if (!format || !output) return false;
    if (!node.type) return xr_xir_value_format_cursor(node.value,output);
    if (node.type>XR_VALUE_FORMAT_MAX_DEPTH+1 || node.value!=&format->cursors[node.type-1]) return false;
    *output=format->cursors[node.type-1];return true;
}
static int xir_format_scalar(const XrXirValue *value,XrValueFormatView *view) {
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
static int xir_format_read(const void *context, XrValueFormatNode node, XrValueFormatView *view) {
    XrXirFormatCursor cursor={0};XrXirValue borrowed={0};
    if (!view || !xir_format_cursor(context,node,&cursor)) return 0;
    if (xr_xir_value_format_leaf(&cursor,&borrowed) && xir_format_scalar(&borrowed,view)) return 1;
    XrXirFormatAggregate aggregate={0};
    if (!xr_xir_value_format_aggregate(&cursor,&aggregate)) return 0;
    view->kind=aggregate.array ? XR_VALUE_FORMAT_ARRAY : XR_VALUE_FORMAT_ENUM;
    view->name=aggregate.name.bytes;view->name_size=aggregate.name.length;
    view->member=aggregate.member.bytes;view->member_size=aggregate.member.length;
    view->unsigned_value=aggregate.count;
    view->children=aggregate.array ? (uint32_t)xr_value_format_limit_count((int64_t)aggregate.count) : (uint32_t)aggregate.count;
    return 1;
}

static int xir_format_child(const void *context, XrValueFormatNode node, uint32_t index,
    XrValueFormatNode *child) {
    XrXirFormatContext *format=(XrXirFormatContext *)context;
    XrXirFormatCursor cursor={0},next={0};
    if (!child || !xir_format_cursor(context,node,&cursor) || node.type>XR_VALUE_FORMAT_MAX_DEPTH ||
        !xr_xir_value_format_child(&cursor,index,&next)) return 0;
    format->cursors[node.type]=next;
    *child=(XrValueFormatNode){&format->cursors[node.type],(uint16_t)(node.type+1)};
    return 1;
}

XR_FUNC XrValueFormatReader xr_xir_value_format_reader(XrXirFormatContext *context) {
    return context ? (XrValueFormatReader){context,xir_format_read,xir_format_child} : (XrValueFormatReader){0};
}
