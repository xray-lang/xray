/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_pod.c - Borrowed inline values through the canonical text formatter
 */
#include "xray_native_pod.h"
#include "xnative_string.h"
#include "base/xchecks.h"
#include "xir/xxir_format.h"
#include "xir/xxir_value_internal.h"
#include "xir/xxir_types.h"
#include <string.h>

typedef struct PodDisplay {
    const XrPodType *types;
    uint32_t count, type;
    XrXirValueAdmission admission;
    XrXirValueStatus status;
} PodDisplay;

static bool pod_layout(const PodDisplay *context, uint32_t type, XrPodLayout *output) {
    if (type < XR_POD_SCALAR_COUNT) return xr_pod_scalar_layout((XrPodScalar)type, output);
    uint32_t index = type - XR_POD_SCALAR_COUNT;
    if (index >= context->count || !context->types) return false;
    *output = context->types[index].physical;
    return true;
}
static bool pod_display_shape(PodDisplay *context, size_t size) {
    uint32_t type = context->type;
    XrPodLayout root;
    if (!pod_layout(context, type, &root) || root.size != size) return false;
#if XR_XIR_PARANOID
    while (type >= XR_POD_SCALAR_COUNT) {
        uint32_t index = type - XR_POD_SCALAR_COUNT;
        const XrPodType *row = &context->types[index];
        XrPodLayout payload, expected;
        if (row->kind != XR_POD_NULLABLE || row->count || row->element >= type ||
            !pod_layout(context, row->element, &payload) || !xr_pod_nullable_layout(payload, &expected) ||
            memcmp(&row->physical, &expected, sizeof(expected))) return false;
        if (!xr_xir_domain_work(context->admission.domain, 1)) {
            context->status = XR_XIR_VALUE_LIMIT;
            return false;
        }
        type = row->element;
    }
#endif
    return true;
}
static int pod_display_read(const void *opaque, XrValueFormatNode node, XrValueFormatView *view) {
    PodDisplay *context = (PodDisplay *)opaque;
    uint32_t type = context->type;
    const uint8_t *bytes = node.value;
    while (type >= XR_POD_SCALAR_COUNT) {
        if (!xr_xir_domain_work(context->admission.domain, 1)) {
            context->status = XR_XIR_VALUE_LIMIT;
            return 0;
        }
        if (*bytes > 1) return 0;
        if (!*bytes) {
            view->kind = XR_VALUE_FORMAT_LITERAL;
            view->name = "null";
            view->name_size = 4;
            return 1;
        }
        const XrPodType *row = &context->types[type - XR_POD_SCALAR_COUNT];
        XrPodLayout payload;
        if (!pod_layout(context, row->element, &payload)) return 0;
        bytes += payload.alignment;
        type = row->element;
    }
    static const XrXirType scalar_types[] = {XR_XIR_UNIT, XR_XIR_BOOL,
        XR_XIR_I8, XR_XIR_I16, XR_XIR_I32, XR_XIR_I64,
        XR_XIR_U8, XR_XIR_U16, XR_XIR_U32, XR_XIR_U64, XR_XIR_F32, XR_XIR_F64};
    XrPodLayout scalar;
    if (!pod_layout(context, type, &scalar)) return 0;
    uint64_t bits = 0;
    if (scalar.size) memcpy(&bits, bytes, scalar.size);
    XrXirType exact = scalar_types[type];
    if (xr_xir_integer_signed(exact) && scalar.size < 8) {
        uint64_t mask = (UINT64_C(1) << (scalar.size * 8)) - 1;
        if (bits > (mask >> 1)) bits |= ~mask;
    }
    XrXirValue value = {exact, 0, bits <= INT64_MAX ? (int64_t)bits : -1 - (int64_t)~bits};
    return (exact == XR_XIR_UNIT || xr_xir_value_argument(&value, NULL, exact)) &&
        xr_xir_format_scalar(&value, view);
}

XRAY_API XR_NOINLINE XrtString *xrt_pod_display(const XrPodType *types, uint32_t count,
    uint32_t type, const void *bytes, size_t size, uint32_t source) {
    XrtValueRoot *root = xrt_value_root_current(source);
    PodDisplay context = {types, count, type,
        {NULL, xrt_value_root_domain(root, source), NULL, NULL, UINT64_MAX, UINT64_MAX}, XR_XIR_VALUE_OK};
    XR_CHECK(!root->pending.type && !root->pending.reserved && !root->pending.payload,
        "inline display publishes one distinct owned String");
    if ((size && !bytes) || (count && !types) || !pod_display_shape(&context, size))
        xrt_value_failure(context.status == XR_XIR_VALUE_OK ? XR_XIR_VALUE_BAD_ARGUMENT : context.status, source);
    XrValueFormatReader reader = {&context, pod_display_read, NULL};
    XrXirValueStatus status = xr_xir_format_text(reader, (XrValueFormatNode){bytes, 0},
        &context.admission, &root->pending);
    if (status != XR_XIR_VALUE_OK)
        xrt_value_failure(context.status == XR_XIR_VALUE_OK ? status : context.status, source);
    return xrt_string_publish_pending(source);
}
