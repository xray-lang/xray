/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_record_metadata.inc.c - Canonical record descriptions and owned names
 */
#include "xxir_record_names_internal.h"
static XrXirStatus record_type_payload(const XrXirTypes *types, uint32_t index,
    XrXirCompileContext *remaining, uint32_t *span) {
    const XrXirTypeNode *node = &types->nodes[index];
    if (node->element != XR_XIR_UNIT || node->result != XR_XIR_UNIT || node->flags ||
        (!!node->parameter_count != !!node->parameters) || (!!node->parameter_count != !!node->record.names))
        return XR_XIR_BAD_STRUCTURE;
    if (node->parameter_count > 65536 || node->parameter_count > remaining->limits.parameters ||
        !xir_compile_work(remaining, node->parameter_count)) return XR_XIR_BUDGET;
    remaining->limits.parameters -= node->parameter_count;
    for (uint32_t p = 0; p < node->parameter_count; ++p) {
        XrXirStatus status = xir_record_name_validate(remaining, node->record.names[p]);
        if (status != XR_XIR_OK) return status;
        if (p) {
            int comparison = 0;
            status = xir_record_name_compare(remaining, node->record.names[p-1], node->record.names[p], &comparison);
            if (status != XR_XIR_OK) return status;
            if (comparison >= 0) return XR_XIR_BAD_STRUCTURE;
        }
        XrXirType field = node->parameters[p].type;
        if (node->parameters[p].mode || (field != XR_XIR_UNIT && !callable_component(types, field, index)))
            return XR_XIR_BAD_TYPE;
        uint32_t component = xr_xir_type_span(types, field);
        if (component > *span) *span = component;
    }
    return XR_XIR_OK;
}
static XrXirStatus record_type_same(const XrXirCompileContext *context,
    const XrXirTypeNode *left, const XrXirTypeNode *right, bool *same) {
    for (uint32_t p = 0; p < left->parameter_count; ++p) {
        int comparison = 0;
        XrXirStatus status = xir_record_name_compare(context, left->record.names[p], right->record.names[p], &comparison);
        if (status != XR_XIR_OK) return status;
        if (comparison) {*same = false;return XR_XIR_OK;}
    }
    return XR_XIR_OK;
}
static void record_type_names_free(const XrXirTypeNode *node) {
    for (uint32_t p = 0; node->record.names && p < node->parameter_count; ++p)
        xr_compile_resources_free((void *)node->record.names[p].bytes);
    xr_compile_resources_free((void *)node->record.names);
}
static XrXirStatus record_type_names_copy(const XrXirCompileContext *context,
    const XrXirTypeNode *source, XrXirTypeNode *target) {
    XrXirStatus status = XR_XIR_OK;
    XrXirLiteral *names = source->parameter_count ? xir_compile_calloc(context,
        source->parameter_count, sizeof(*names), &status) : NULL;
    target->record.names = names;
    if (status != XR_XIR_OK) return status;
    for (uint32_t p = 0; p < source->parameter_count; ++p) {
        XrXirLiteral name = source->record.names[p];
        char *bytes = xir_compile_alloc(context, (size_t)name.length + 1, &status);
        if (!bytes) return status;
        names[p] = (XrXirLiteral){bytes, name.length};
        if (!xir_compile_work(context, (uint64_t)name.length + 1)) return XR_XIR_BUDGET;
        memcpy(bytes, name.bytes, name.length);bytes[name.length] = 0;
    }
    return status;
}
