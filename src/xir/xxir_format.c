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
#include "xxir_value_internal.h"
#include "xxir_type_arena_internal.h"

static int xir_format_read(const void *context, XrValueFormatNode node, XrValueFormatView *view) {
    XrXirValue borrowed = {0};
    if (!view || !xr_xir_value_format_storage(context, node, &borrowed, view)) return 0;
    if (view->kind) return 1;
    const XrXirValue *value = &borrowed;
    XrXirType type = (XrXirType)value->type;
    if (type == XR_XIR_UNIT) {
        view->kind = XR_VALUE_FORMAT_LITERAL; view->name = "()"; view->name_size = 2;
        return 1;
    }
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

typedef struct MeteredFormat {
    XrXirValueAdmission *admission;
    XrValueFormatReader borrowed;
    XrXirValueStatus status;
    char *bytes;
    size_t used, limit;
} MeteredFormat;
static bool metered_format_work(MeteredFormat *format, uint64_t work) {
    if (work>format->admission->work || !xr_xir_domain_work(format->admission->domain,work)) {
        format->status=XR_XIR_VALUE_LIMIT; return false;
    }
    format->admission->work-=work; return true;
}
static int metered_format_read(const void *context, XrValueFormatNode node, XrValueFormatView *view) {
    MeteredFormat *format=(MeteredFormat *)context;
    return metered_format_work(format,1) && format->borrowed.read(format->borrowed.context,node,view);
}
static int metered_format_child(const void *context, XrValueFormatNode node, uint32_t index,
    XrValueFormatNode *child) {
    MeteredFormat *format=(MeteredFormat *)context;
    return metered_format_work(format,1) && format->borrowed.child(format->borrowed.context,node,index,child);
}
static int metered_format_write(void *context, const void *bytes, size_t length) {
    MeteredFormat *format=context;
    if (length>format->limit-format->used || length==SIZE_MAX) {
        format->status=XR_XIR_VALUE_LIMIT; return 0;
    }
    if (!metered_format_work(format,(uint64_t)length+1)) return 0;
    if (format->bytes && length) memcpy(format->bytes+format->used,bytes,length);
    format->used+=length; return 1;
}
XR_FUNC XrXirValueStatus xr_xir_value_format_string(const XrXirValue *value,
    XrXirValueAdmission *admission, XrXirValue *output) {
    if (!admission || !xr_xir_domain_current(admission->domain) || !value || !output ||
        output->type || output->reserved || output->payload) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirType expected=(XrXirType)value->type;
    const XrXirTypeArena *owner=xr_xir_value_arena(value);
    if (owner && owner!=admission->arena) {
        uint64_t work=xir_type_arena_lookup_work(admission->arena);
        if (work>admission->work || !xr_xir_domain_work(admission->domain,work)) return XR_XIR_VALUE_LIMIT;
        admission->work-=work;
        if (!xir_type_arena_local_type(admission->arena,owner,expected,&expected)) return XR_XIR_VALUE_BAD_ARGUMENT;
    }
    XrXirValueStatus status=xr_xir_value_admit(value,expected,admission);
    if (status!=XR_XIR_VALUE_OK) return status;
    MeteredFormat format={admission,xr_xir_value_format_reader(value),XR_XIR_VALUE_OK,NULL,0,
        admission->scratch_bytes>SIZE_MAX ? SIZE_MAX : (size_t)admission->scratch_bytes};
    XrValueFormatReader reader={&format,metered_format_read,metered_format_child};
    XrValueFormatSink sink={&format,metered_format_write};
    if (!xr_value_format_value(reader,(XrValueFormatNode){value,0, NULL},sink,0))
        return format.status==XR_XIR_VALUE_OK ? XR_XIR_VALUE_BAD_ARGUMENT : format.status;
    size_t length=format.used, scratch=length ? length : 1;
    if (scratch>admission->scratch_bytes) return XR_XIR_VALUE_LIMIT;
    format.bytes=xr_xir_domain_allocate(admission->domain,scratch,&status);
    if (!format.bytes) return status;
    admission->scratch_bytes-=scratch; format.limit=length; format.used=0;
    if (!xr_value_format_value(reader,(XrValueFormatNode){value,0, NULL},sink,0) || format.used!=length)
        status=format.status==XR_XIR_VALUE_OK ? XR_XIR_VALUE_BAD_ARGUMENT : format.status;
    else status=xr_xir_string_new(admission->domain,format.bytes,length,output);
    xr_xir_domain_deallocate(admission->domain,format.bytes,scratch);
    admission->scratch_bytes+=scratch; return status;
}
