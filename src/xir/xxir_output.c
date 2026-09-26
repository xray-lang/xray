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
#include "../base/xmalloc.h"
#include <stdio.h>

static bool output_piece(const XrXirValue *value, char *scalar, const char **bytes, size_t *length) {
    if (!xr_xir_value_argument(value, (XrXirType) value->type)) return false;
    if (value->type == XR_XIR_STRING) return xr_xir_string_view(value, bytes, length);
    if (value->type == XR_XIR_BOOL) {
        *bytes = value->payload ? "true" : "false"; *length = value->payload ? 4 : 5; return true;
    }
    if (value->type != XR_XIR_I64) return false;
    int count = snprintf(scalar, 32, "%lld", (long long) value->payload);
    if (count <= 0 || count >= 32) return false;
    *bytes = scalar; *length = (size_t) count; return true;
}
bool xr_xir_output_render(void *context, const XrXirOutputGroup *group) {
    XrXirOutputSink *sink = context;
    if (!sink || !sink->write || !group ||
        (group->stream != XR_XIR_STDOUT && group->stream != XR_XIR_STDERR) ||
        (group->line && group->stream != XR_XIR_STDOUT) ||
        (group->count && !group->values) || group->count > 65536 ||
        (!group->line && group->count != 1)) return false;
    size_t total = group->line ? (group->count ? group->count : 1) : 0;
    if (total > sink->byte_limit) return false;
    for (uint32_t i = 0; i < group->count; ++i) {
        char scalar[32]; const char *bytes = NULL; size_t length = 0;
        if (!output_piece(&group->values[i], scalar, &bytes, &length) ||
            length > sink->byte_limit - total) return false;
        total += length;
    }
    char *buffer = xr_malloc(total ? total : 1);
    if (!buffer) return false;
    size_t used = 0;
    for (uint32_t i = 0; i < group->count; ++i) {
        char scalar[32]; const char *bytes = NULL; size_t length = 0;
        size_t separator = group->line && i ? 1 : 0;
        if (!output_piece(&group->values[i], scalar, &bytes, &length) ||
            separator > total - used || length > total - used - separator) {
            xr_free(buffer); return false;
        }
        if (group->line && i) buffer[used++] = ' ';
        memcpy(buffer + used, bytes, length); used += length;
    }
    if (group->line) {
        if (used == total) { xr_free(buffer); return false; }
        buffer[used++] = '\n';
    }
    if (used != total) { xr_free(buffer); return false; }
    bool accepted = sink->write(sink->context, group->stream, buffer, used);
    xr_free(buffer);
    return accepted;
}
