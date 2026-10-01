/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_output.c - Prepare complete output before publishing bytes
 *
 * KEY CONCEPT:
 *   Scalar formatting is deterministic and strings retain their exact length.
 */
#include "xxir_output.h"
#include "xxir_float.h"
#include "../base/xmalloc.h"
#include <stdio.h>

static bool output_piece(const XrXirValue *value, char *scalar, const char **bytes, size_t *length) {
    if (!xr_xir_value_argument(value, NULL, (XrXirType) value->type)) return false;
    if (value->type == XR_XIR_STRING) return xr_xir_string_view(value, bytes, length);
    if (value->type == XR_XIR_BOOL) {
        *bytes = value->payload ? "true" : "false"; *length = value->payload ? 4 : 5; return true;
    }
    uint32_t floating = xr_xir_float_bits((XrXirType) value->type);
    if (floating) {
        *bytes = scalar;
        return xr_xir_float_format(floating, (uint64_t) value->payload, scalar, 32, length);
    }
    if (!xr_xir_type_is_integer((XrXirType) value->type)) return false;
    int count = xr_xir_integer_signed((XrXirType) value->type) ?
        snprintf(scalar, 32, "%lld", (long long) value->payload) :
        snprintf(scalar, 32, "%llu", (unsigned long long) (uint64_t) value->payload);
    if (count <= 0 || count >= 32) return false;
    *bytes = scalar; *length = (size_t) count; return true;
}
XrXirOutputStatus xr_xir_output_render(void *context, const XrXirOutputGroup *group) {
    XrXirOutputSink *sink = context;
    if (!sink) return XR_XIR_OUTPUT_BAD_ARGUMENT;
    if (sink->abi_version != XR_XIR_CALL_ABI_VERSION || sink->reserved) return XR_XIR_OUTPUT_BAD_ABI;
    if (!sink->write || !group ||
        (group->stream != XR_XIR_STDOUT && group->stream != XR_XIR_STDERR) ||
        (group->line && group->stream != XR_XIR_STDOUT) ||
        (group->count && !group->values) || group->count > 65536 ||
        (!group->line && group->count != 1)) return XR_XIR_OUTPUT_BAD_ARGUMENT;
    size_t total = group->line ? (group->count ? group->count : 1) : 0;
    if (total > sink->byte_limit) return XR_XIR_OUTPUT_LIMIT;
    for (uint32_t i = 0; i < group->count; ++i) {
        char scalar[32]; const char *bytes = NULL; size_t length = 0;
        if (!output_piece(&group->values[i], scalar, &bytes, &length)) return XR_XIR_OUTPUT_BAD_ARGUMENT;
        if (length > sink->byte_limit - total) return XR_XIR_OUTPUT_LIMIT;
        total += length;
    }
    char *buffer = xr_malloc(total ? total : 1);
    if (!buffer) return XR_XIR_OUTPUT_OOM;
    size_t used = 0;
    for (uint32_t i = 0; i < group->count; ++i) {
        char scalar[32]; const char *bytes = NULL; size_t length = 0;
        size_t separator = group->line && i ? 1 : 0;
        if (!output_piece(&group->values[i], scalar, &bytes, &length) ||
            separator > total - used || length > total - used - separator) {
            xr_free(buffer); return XR_XIR_OUTPUT_BAD_ARGUMENT;
        }
        if (group->line && i) buffer[used++] = ' ';
        memcpy(buffer + used, bytes, length); used += length;
    }
    if (group->line) {
        if (used == total) { xr_free(buffer); return XR_XIR_OUTPUT_BAD_ARGUMENT; }
        buffer[used++] = '\n';
    }
    if (used != total) { xr_free(buffer); return XR_XIR_OUTPUT_BAD_ARGUMENT; }
    XrXirOutputStatus status = sink->write(sink->context, group->stream, buffer, used);
    xr_free(buffer);
    return status >= XR_XIR_OUTPUT_OK && status <= XR_XIR_OUTPUT_BAD_ABI ? status : XR_XIR_OUTPUT_BAD_ARGUMENT;
}
