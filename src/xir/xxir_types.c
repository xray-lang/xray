/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_types.c - Bounded structural type admission and owned snapshots
 *
 * KEY CONCEPT:
 *   Earlier node edges form a finite graph whose exact contracts have one identity.
 */
#include "xxir_types.h"
#include "../base/xmalloc.h"

const XrXirTypeNode *xr_xir_type_node(const XrXirTypes *types, XrXirType type) {
    uint32_t id = (uint32_t) type;
    return types && types->nodes && id >= XR_XIR_CONSTRUCTED_TYPE_BASE &&
        id < XR_XIR_CONSTRUCTED_TYPE_LIMIT && id - XR_XIR_CONSTRUCTED_TYPE_BASE < types->count ?
        &types->nodes[id - XR_XIR_CONSTRUCTED_TYPE_BASE] : NULL;
}
static bool type_has_kind(const XrXirTypes *types, XrXirType type, uint32_t kind) {
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    return node && node->kind == kind;
}
bool xr_xir_type_is_callable(const XrXirTypes *types, XrXirType type) {
    return type_has_kind(types, type, XR_XIR_TYPE_CALLABLE);
}
bool xr_xir_type_is_array(const XrXirTypes *types, XrXirType type) {
    return type_has_kind(types, type, XR_XIR_TYPE_ARRAY);
}
bool xr_xir_type_is_cell(const XrXirTypes *types, XrXirType type) {
    return type_has_kind(types, type, XR_XIR_TYPE_CELL);
}
bool xr_xir_type_is_owned(const XrXirTypes *types, XrXirType type) {
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    return type == XR_XIR_STRING || type == XR_XIR_ATOMIC_I64 ||
        (node && (node->kind == XR_XIR_TYPE_CALLABLE || node->kind == XR_XIR_TYPE_ARRAY ||
                  node->kind == XR_XIR_TYPE_CELL));
}
const XrXirTypeNode *xr_xir_callable_signature(const XrXirTypes *types, XrXirType type) {
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    return node && node->kind == XR_XIR_TYPE_CALLABLE ? node : NULL;
}
static XrXirType type_element(const XrXirTypes *types, XrXirType type, uint32_t kind) {
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    return node && node->kind == kind ? node->element : XR_XIR_UNIT;
}
XrXirType xr_xir_cell_element(const XrXirTypes *types, XrXirType type) {
    return type_element(types, type, XR_XIR_TYPE_CELL);
}
XrXirType xr_xir_array_element(const XrXirTypes *types, XrXirType type) {
    return type_element(types, type, XR_XIR_TYPE_ARRAY);
}
uint32_t xr_xir_type_span(const XrXirTypes *types, XrXirType type) {
    uint32_t id = (uint32_t) type;
    if (id >= XR_XIR_TYPE_PARAMETER_BASE && id < XR_XIR_TYPE_PARAMETER_LIMIT)
        return id - XR_XIR_TYPE_PARAMETER_BASE + 1;
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    return node ? node->parameter_span : 0;
}
XrXirStatus xr_xir_type_sendable(const XrXirTypes *types, XrXirType type,
    const uint32_t *constraints, uint32_t parameter_count, uint64_t *work) {
    for (;;) {
        if (!work || !*work) return XR_XIR_BUDGET;
        --*work;
        uint32_t id = (uint32_t) type;
        if (type == XR_XIR_BOOL || xr_xir_type_is_number(type) || type == XR_XIR_STRING ||
            type == XR_XIR_ATOMIC_I64) return XR_XIR_OK;
        if (id >= XR_XIR_TYPE_PARAMETER_BASE && id < XR_XIR_TYPE_PARAMETER_LIMIT)
            return constraints && id - XR_XIR_TYPE_PARAMETER_BASE < parameter_count &&
                (constraints[id - XR_XIR_TYPE_PARAMETER_BASE] & XR_XIR_CONSTRAINT_SENDABLE) ? XR_XIR_OK : XR_XIR_BAD_TYPE;
        const XrXirTypeNode *node = xr_xir_type_node(types, type);
        if (!node || node->kind != XR_XIR_TYPE_ARRAY) return XR_XIR_BAD_TYPE;
        if ((uint32_t) node->element >= XR_XIR_CONSTRUCTED_TYPE_BASE &&
            (uint32_t) node->element < XR_XIR_CONSTRUCTED_TYPE_LIMIT && (uint32_t) node->element >= id)
            return XR_XIR_BAD_TYPE;
        type = node->element;
    }
}
static bool type_component(const XrXirTypes *types, XrXirType type, uint32_t earlier) {
    uint32_t id = (uint32_t) type;
    if (type == XR_XIR_BOOL || xr_xir_type_is_number(type) || type == XR_XIR_STRING ||
        type == XR_XIR_ATOMIC_I64) return true;
    if (id >= XR_XIR_TYPE_PARAMETER_BASE && id < XR_XIR_TYPE_PARAMETER_LIMIT) return true;
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    return node && id - XR_XIR_CONSTRUCTED_TYPE_BASE < earlier &&
        (node->kind == XR_XIR_TYPE_CALLABLE || node->kind == XR_XIR_TYPE_ARRAY);
}
static XrXirStatus type_payload(const XrXirTypes *types, uint32_t index, XrXirBudget *remaining) {
    const XrXirTypeNode *node = &types->nodes[index];
    uint32_t span = 0;
    if (node->kind == XR_XIR_TYPE_CALLABLE) {
        if (node->element != XR_XIR_UNIT || node->flags ||
            (node->result != XR_XIR_UNIT && !type_component(types, node->result, index))) return XR_XIR_BAD_TYPE;
        if (node->parameter_count > 65536 || node->parameter_count > remaining->parameters) return XR_XIR_BUDGET;
        uint64_t bytes = (uint64_t) node->parameter_count * sizeof(*node->parameters);
        if (bytes > SIZE_MAX || bytes > remaining->metadata_bytes || node->parameter_count > remaining->work)
            return XR_XIR_BUDGET;
        remaining->metadata_bytes -= bytes; remaining->work -= node->parameter_count;
        remaining->parameters -= node->parameter_count;
        if ((node->parameter_count != 0) != (node->parameters != NULL)) return XR_XIR_BAD_STRUCTURE;
        span = xr_xir_type_span(types, node->result);
        for (uint32_t p = 0; p < node->parameter_count; ++p) {
            if (node->parameters[p].mode || !type_component(types, node->parameters[p].type, index)) return XR_XIR_BAD_TYPE;
            uint32_t component = xr_xir_type_span(types, node->parameters[p].type);
            if (component > span) span = component;
        }
    } else if (node->kind == XR_XIR_TYPE_ARRAY || node->kind == XR_XIR_TYPE_CELL) {
        if (node->parameters || node->parameter_count || node->result != XR_XIR_UNIT || node->flags)
            return XR_XIR_BAD_STRUCTURE;
        if (!type_component(types, node->element, index)) return XR_XIR_BAD_TYPE;
        span = xr_xir_type_span(types, node->element);
    } else return XR_XIR_BAD_TYPE;
    return node->parameter_span == span ? XR_XIR_OK : XR_XIR_BAD_TYPE;
}
static XrXirStatus type_unique(const XrXirTypes *types, uint32_t index, uint64_t *work) {
    const XrXirTypeNode *node = &types->nodes[index];
    for (uint32_t j = 0; j < index; ++j) {
        if (!*work) return XR_XIR_BUDGET;
        --*work;
        const XrXirTypeNode *previous = &types->nodes[j];
        if (previous->kind != node->kind || previous->element != node->element ||
            previous->parameter_count != node->parameter_count || previous->result != node->result ||
            previous->flags != node->flags) continue;
        if (node->parameter_count > *work) return XR_XIR_BUDGET;
        *work -= node->parameter_count;
        bool same = true;
        for (uint32_t p = 0; p < node->parameter_count; ++p)
            if (node->parameters[p].type != previous->parameters[p].type ||
                node->parameters[p].mode != previous->parameters[p].mode) same = false;
        if (same) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
XrXirStatus xr_xir_types_verify(const XrXirTypes *types, XrXirBudget *remaining) {
    if (!types) return XR_XIR_OK;
    if (!remaining || !types->count || !types->nodes) return XR_XIR_BAD_STRUCTURE;
    if (types->count > XR_XIR_CONSTRUCTED_TYPE_LIMIT - XR_XIR_CONSTRUCTED_TYPE_BASE) return XR_XIR_BUDGET;
    uint64_t bytes = sizeof(*types) + (uint64_t) types->count * sizeof(*types->nodes);
    if (bytes > SIZE_MAX || bytes > remaining->metadata_bytes || types->count > remaining->work) return XR_XIR_BUDGET;
    remaining->metadata_bytes -= bytes; remaining->work -= types->count;
    for (uint32_t i = 0; i < types->count; ++i) {
        XrXirStatus status = type_payload(types, i, remaining);
        if (status == XR_XIR_OK) status = type_unique(types, i, &remaining->work);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
void xr_xir_types_free(XrXirTypes *types) {
    if (!types) return;
    if (types->nodes)
        for (uint32_t i = 0; i < types->count; ++i) xr_free((void *) types->nodes[i].parameters);
    xr_free((void *) types->nodes); xr_free(types);
}
XrXirStatus xr_xir_types_clone(const XrXirTypes *types, XrXirTypes **output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    *output = NULL;
    if (!types) return XR_XIR_OK;
    if (!types->nodes || !types->count ||
        types->count > XR_XIR_CONSTRUCTED_TYPE_LIMIT - XR_XIR_CONSTRUCTED_TYPE_BASE) return XR_XIR_BAD_STRUCTURE;
    XrXirTypes *copy = xr_calloc(1, sizeof(*copy));
    if (!copy) return XR_XIR_OUT_OF_MEMORY;
    XrXirTypeNode *nodes = xr_calloc(types->count, sizeof(*nodes));
    if (!nodes) { xr_free(copy); return XR_XIR_OUT_OF_MEMORY; }
    copy->nodes = nodes; copy->count = types->count;
    for (uint32_t i = 0; i < types->count; ++i) {
        nodes[i] = types->nodes[i];
        uint32_t count = nodes[i].parameter_count;
        XrXirCallableParameter *parameters = count ? xr_malloc((size_t) count * sizeof(*parameters)) : NULL;
        nodes[i].parameters = parameters;
        if (count && !parameters) { xr_xir_types_free(copy); return XR_XIR_OUT_OF_MEMORY; }
        if (count) memcpy(parameters, types->nodes[i].parameters, (size_t) count * sizeof(*parameters));
    }
    *output = copy; return XR_XIR_OK;
}
XrXirType xr_xir_operand_type(const XrXirFunction *function, uint32_t value) {
    if (value < function->parameter_count) return function->parameters[value];
    value -= function->parameter_count;
    return value < function->instruction_count ? function->instructions[value].type : XR_XIR_UNIT;
}
