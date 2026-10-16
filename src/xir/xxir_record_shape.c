/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_record_shape.c - Finite canonical field preparation without evaluation
 */
#include "xxir_record_shape.h"
#include "xxir_types.h"
#include "xxir_compile_memory.h"
#include "xxir_type_scratch_internal.h"
#include "xxir_record_names_internal.h"
#include "../base/xchecks.h"
static XrXirStatus record_shape_order(const XrXirCompileContext *context,
    const XrXirRecordShapeInput *input, uint32_t *order, uint32_t *temporary) {
    if (!xir_compile_work(context, (uint64_t)input->count * sizeof(*order))) return XR_XIR_BUDGET;
    for (uint32_t i = 0; i < input->count; ++i) order[i] = i;
    for (uint32_t width = 1; width < input->count; width *= 2) {
        if (!xir_compile_work(context, (uint64_t)input->count * 2 * sizeof(*order))) return XR_XIR_BUDGET;
        for (uint32_t begin = 0; begin < input->count; begin += width * 2) {
            uint32_t middle = begin + width < input->count ? begin + width : input->count;
            uint32_t end = middle + width < input->count ? middle + width : input->count;
            uint32_t left = begin, right = middle;
            for (uint32_t at = begin; at < end; ++at) {
                int comparison = -1;
                if (left < middle && right < end) {
                    XrXirStatus status = xir_record_name_compare(context, input->fields[order[left]].name,
                        input->fields[order[right]].name, &comparison);
                    if (status != XR_XIR_OK) return status;
                }
                temporary[at] = left < middle && (right == end || comparison <= 0) ? order[left++] : order[right++];
            }
        }
        memcpy(order, temporary, (size_t)input->count * sizeof(*order));
    }
    for (uint32_t i = 1; i < input->count; ++i) {
        int comparison = 0;
        XrXirStatus status = xir_record_name_compare(context, input->fields[order[i-1]].name,
            input->fields[order[i]].name, &comparison);
        if (status != XR_XIR_OK) return status;
        if (!comparison) return XR_XIR_BAD_STRUCTURE;
        XR_CHECK(comparison < 0, "canonical record names have one strict byte order");
    }
    return XR_XIR_OK;
}
static XrXirStatus record_shape_validate(const XrXirCompileContext *context,
    const XrXirRecordShapeInput *input, uint64_t *name_bytes) {
    XirTypeScratch scratch = {context->resources, NULL, 0};
    XrXirStatus status = XR_XIR_OK;
    *name_bytes = 0;
    for (uint32_t i = 0; i < input->count && status == XR_XIR_OK; ++i) {
        if (!xir_compile_work(context, sizeof(*input->fields))) {status = XR_XIR_BUDGET;break;}
        const XrXirRecordField *field = &input->fields[i];
        status = xir_record_name_validate(context, field->name);
        if (status != XR_XIR_OK) break;
        if (UINT64_MAX - *name_bytes <= field->name.length) {status = XR_XIR_BUDGET;break;}
        *name_bytes += (uint64_t)field->name.length + 1;
        if (xr_xir_type_is_cell(input->types, field->type)) {status = XR_XIR_BAD_TYPE;break;}
        status = xr_xir_compile_type_expression_shape_scratch(context, input->types, field->type,
            input->parameter_count, &scratch);
    }
    xir_type_scratch_free(&scratch);
    return status;
}
static XrXirStatus record_shape_copy(const XrXirCompileContext *context,
    const XrXirRecordShapeInput *input, const uint32_t *order, uint64_t names, XrXirRecordShape **output) {
    uint64_t arrays = (uint64_t)input->count * (sizeof(XrXirRecordField) + sizeof(uint32_t));
    if (names > SIZE_MAX - sizeof(XrXirRecordShape) - arrays) return XR_XIR_BUDGET;
    XrXirStatus status = XR_XIR_OK;
    XrXirRecordShape *shape = xir_compile_calloc(context, 1, sizeof(*shape) + (size_t)arrays + (size_t)names, &status);
    if (!shape) return status;
    _Static_assert(_Alignof(XrXirRecordShape) >= _Alignof(XrXirRecordField), "owned fields follow an aligned shape");
    XrXirRecordField *fields = input->count ? (XrXirRecordField *)(shape + 1) : NULL;
    uint32_t *source = input->count ? (uint32_t *)(fields + input->count) : NULL;
    char *text = input->count ? (char *)(source + input->count) : NULL;
    if (!xir_compile_work(context, sizeof(*shape))) {status = XR_XIR_BUDGET;goto failed;}
    shape->fields = fields;shape->source_to_field = source;shape->count = input->count;
    for (uint32_t i = 0; i < input->count; ++i) {
        XrXirRecordField field = input->fields[order[i]];
        if (!xir_compile_work(context, sizeof(field) + sizeof(*source) + (uint64_t)field.name.length + 1)) {
            status = XR_XIR_BUDGET;goto failed;
        }
        memcpy(text, field.name.bytes, field.name.length);text[field.name.length] = 0;
        field.name.bytes = text;fields[i] = field;source[order[i]] = i;text += (size_t)field.name.length + 1;
    }
    if (!xir_compile_work(context, sizeof(*output))) {status = XR_XIR_BUDGET;goto failed;}
    *output = shape;return XR_XIR_OK;
failed:
    xr_compile_resources_free(shape);return status;
}
XR_FUNC XrXirStatus xr_xir_compile_record_shape_new(const XrXirCompileContext *context,
    const XrXirRecordShapeInput *input, XrXirRecordShape **output) {
    if (!xir_compile_context_valid(context) || !input || !output || *output ||
        ((input->count != 0) != (input->fields != NULL))) return XR_XIR_BAD_STRUCTURE;
    if (input->count > 65536 || input->count > context->limits.parameters ||
        input->parameter_count > 65536 || input->parameter_count > context->limits.parameters) return XR_XIR_BUDGET;
    uint64_t names = 0;
    XrXirStatus status = record_shape_validate(context, input, &names);
    if (status != XR_XIR_OK) return status;
    uint32_t *order = input->count ? xir_compile_alloc(context, (size_t)input->count * 2 * sizeof(*order), &status) : NULL;
    if (status == XR_XIR_OK && input->count)
        status = record_shape_order(context, input, order, order + input->count);
    if (status == XR_XIR_OK) status = record_shape_copy(context, input, order, names, output);
    xr_compile_resources_free(order);return status;
}
