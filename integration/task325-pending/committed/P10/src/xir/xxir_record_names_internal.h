/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_record_names_internal.h - One charged identifier and canonical byte order
 */
#ifndef XXIR_RECORD_NAMES_INTERNAL_H
#define XXIR_RECORD_NAMES_INTERNAL_H
#include "xxir_compile_memory.h"
#include "../shared/xr_utf8_core.h"
static inline int xir_record_name_read(void *owner, const uint8_t *address, uint8_t *output) {
    if (xr_compile_resources_work(owner, 1) != XR_COMPILE_RESOURCE_OK) return 0;
    *output = *address;return 1;
}
static inline XrXirStatus xir_record_name_validate(const XrXirCompileContext *context, XrXirLiteral name) {
    if (!name.bytes || !name.length) return XR_XIR_BAD_STRUCTURE;
    size_t offset = 0;
    while (offset < name.length) {
        XrUtf8Step step = {0};
        if (!xr_utf8_core_decode_step_read((const uint8_t *)name.bytes + offset, name.length - offset,
            xir_record_name_read, context->resources, &step)) return XR_XIR_BUDGET;
        if (step.error != XR_UTF8_OK) return XR_XIR_BAD_STRUCTURE;
        if (step.scalar < 128) {
            uint32_t byte = step.scalar;
            bool alpha = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || byte == '_';
            if ((!alpha && (!offset || byte < '0' || byte > '9')) || (name.length == 1 && byte == '_'))
                return XR_XIR_BAD_STRUCTURE;
        }
        offset += step.consumed;
    }
    return XR_XIR_OK;
}
static inline XrXirStatus xir_record_name_compare(const XrXirCompileContext *context,
    XrXirLiteral left, XrXirLiteral right, int *result) {
    uint32_t common = left.length < right.length ? left.length : right.length;
    if (!xir_compile_work(context, (uint64_t)common + 1)) return XR_XIR_BUDGET;
    int comparison = common ? memcmp(left.bytes, right.bytes, common) : 0;
    *result = comparison ? comparison : left.length < right.length ? -1 : left.length > right.length;
    return XR_XIR_OK;
}
static inline XrXirStatus xir_record_names_match(const XrXirCompileContext *context,
    const XrXirTypeNode *left, const XrXirTypeNode *right, bool *same) {
    if (!left || !right || !same || left->kind != XR_XIR_TYPE_RECORD || right->kind != XR_XIR_TYPE_RECORD ||
        left->parameter_count != right->parameter_count ||
        (left->parameter_count && (!left->record.names || !right->record.names))) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t p = 0; p < left->parameter_count; ++p) {
        int comparison = 0;
        XrXirStatus status = xir_record_name_compare(context, left->record.names[p], right->record.names[p], &comparison);
        if (status != XR_XIR_OK) return status;
        if (comparison) {*same = false;return XR_XIR_OK;}
    }
    return XR_XIR_OK;
}
#endif // XXIR_RECORD_NAMES_INTERNAL_H
