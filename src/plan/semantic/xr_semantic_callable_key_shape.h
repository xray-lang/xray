/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_callable_key_shape.h - Bounded frozen callable signature reader
 *
 * KEY CONCEPT:
 *   A function key owns its complete structural signature. Embedded type keys
 *   are length framed, so nested functions cannot change the outer shape.
 *   Parsing proves structure only; callers separately bind declaration facts.
 */
#ifndef XR_SEMANTIC_CALLABLE_KEY_SHAPE_H
#define XR_SEMANTIC_CALLABLE_KEY_SHAPE_H

#include "xr_semantic_plan.h"
#include "../../runtime/value/xtype.h"
#include <limits.h>
#include <string.h>

typedef struct XrSemanticCallableKeyCursor {
    const char *at;
    const char *end;
} XrSemanticCallableKeyCursor;

typedef struct XrSemanticCallableKeySlice {
    const char *data;
    size_t size;
} XrSemanticCallableKeySlice;

typedef struct XrSemanticCallableKeyShape {
    uint16_t parameter_count;
    uint16_t minimum_parameters;
    uint16_t generic_parameter_count;
    uint16_t view_origin_count;
    uint8_t is_variadic;
    uint8_t is_c_abi;
    uint8_t throw_effect;
    uint8_t receiver_mode;
    uint8_t view_origin_was_elided;
    const char *throw_field;
    const char *parameter_begin;
    const char *generic_begin;
    XrSemanticCallableKeySlice result;
} XrSemanticCallableKeyShape;

static inline bool xr_semantic_callable_key_literal(XrSemanticCallableKeyCursor *cursor,
                                                     const char *literal) {
    size_t count = strlen(literal);
    if (!cursor || cursor->at > cursor->end || count > (size_t) (cursor->end - cursor->at) ||
        memcmp(cursor->at, literal, count) != 0)
        return false;
    cursor->at += count;
    return true;
}

static inline bool xr_semantic_callable_key_unsigned(XrSemanticCallableKeyCursor *cursor,
                                                      unsigned maximum, unsigned *out) {
    if (!cursor || !out || cursor->at >= cursor->end || *cursor->at < '0' || *cursor->at > '9')
        return false;
    const char *start = cursor->at;
    unsigned value = 0;
    while (cursor->at < cursor->end && *cursor->at >= '0' && *cursor->at <= '9') {
        unsigned digit = (unsigned) (*cursor->at - '0');
        if (digit > maximum || value > (maximum - digit) / 10u)
            return false;
        value = value * 10u + digit;
        cursor->at++;
    }
    if (cursor->at - start > 1 && *start == '0')
        return false;
    *out = value;
    return true;
}

static inline bool xr_semantic_callable_key_component(XrSemanticCallableKeyCursor *cursor,
                                                       XrSemanticCallableKeySlice *out) {
    unsigned count = 0;
    if (!out || !xr_semantic_callable_key_unsigned(cursor, UINT_MAX, &count) ||
        !xr_semantic_callable_key_literal(cursor, ":") ||
        count > (size_t) (cursor->end - cursor->at))
        return false;
    *out = (XrSemanticCallableKeySlice) {cursor->at, count};
    cursor->at += count;
    return true;
}

static inline bool xr_semantic_callable_key_parameter(XrSemanticCallableKeyCursor *cursor,
                                                       uint8_t *mode,
                                                       XrSemanticCallableKeySlice *type) {
    unsigned value = 0;
    if (!mode || !xr_semantic_callable_key_literal(cursor, ";p") ||
        !xr_semantic_callable_key_unsigned(cursor, XR_PARAM_MOVE, &value) ||
        !xr_param_mode_is_valid((XrParamMode) value) ||
        !xr_semantic_callable_key_literal(cursor, ":") ||
        !xr_semantic_callable_key_component(cursor, type) || type->size == 0)
        return false;
    *mode = (uint8_t) value;
    return true;
}

static inline bool xr_semantic_callable_key_parse(const char *key,
                                                  XrSemanticCallableKeyShape *out) {
    if (!key || !out)
        return false;
    XrSemanticCallableKeyCursor cursor = {key, key + strlen(key)};
    XrSemanticCallableKeyShape shape = {0};
    unsigned value = 0;
    if (!xr_semantic_callable_key_literal(&cursor, "type-v3:") ||
        !xr_semantic_callable_key_unsigned(&cursor, XR_KIND_COUNT - 1u, &value) ||
        value != XR_KIND_FUNCTION || !xr_semantic_callable_key_literal(&cursor, ":"))
        return false;
    for (unsigned field = 1; field < 10; field++) {
        if (!xr_semantic_callable_key_unsigned(&cursor, UINT_MAX, &value) ||
            !xr_semantic_callable_key_literal(&cursor, ":"))
            return false;
    }
    XrSemanticCallableKeySlice ignored = {0};
    if (!xr_semantic_callable_key_component(&cursor, &ignored) ||
        !xr_semantic_callable_key_literal(&cursor, "fn-v2:"))
        return false;
    unsigned limits[8] = {UINT16_MAX, UINT16_MAX, 1u, 1u, XR_FN_EFFECT_POLY,
                          XR_PARAM_MOVE, 1u, UINT16_MAX};
    unsigned fields[8] = {0};
    for (unsigned field = 0; field < 8; field++) {
        if (field == 4)
            shape.throw_field = cursor.at;
        if (!xr_semantic_callable_key_unsigned(&cursor, limits[field], &fields[field]) ||
            (field < 7 && !xr_semantic_callable_key_literal(&cursor, ":")))
            return false;
    }
    if (fields[1] > fields[0] || !xr_param_mode_is_valid((XrParamMode) fields[5]))
        return false;
    shape.parameter_count = (uint16_t) fields[0];
    shape.minimum_parameters = (uint16_t) fields[1];
    shape.is_variadic = (uint8_t) fields[2];
    shape.is_c_abi = (uint8_t) fields[3];
    shape.throw_effect = (uint8_t) fields[4];
    shape.receiver_mode = (uint8_t) fields[5];
    shape.view_origin_was_elided = (uint8_t) fields[6];
    shape.generic_parameter_count = (uint16_t) fields[7];
    shape.parameter_begin = cursor.at;
    for (unsigned p = 0; p < shape.parameter_count; p++) {
        uint8_t mode = 0;
        if (!xr_semantic_callable_key_parameter(&cursor, &mode, &ignored))
            return false;
    }
    if (!xr_semantic_callable_key_literal(&cursor, ";ret:") ||
        !xr_semantic_callable_key_component(&cursor, &shape.result) || shape.result.size == 0 ||
        !xr_semantic_callable_key_literal(&cursor, ";view-count:") ||
        !xr_semantic_callable_key_unsigned(&cursor, UINT16_MAX, &value))
        return false;
    shape.view_origin_count = (uint16_t) value;
    XrSemanticCallableKeyCursor parameters = {shape.parameter_begin, cursor.end};
    unsigned next_parameter = 0;
    int previous_kind = -1;
    int previous_ordinal = -2;
    for (unsigned v = 0; v < shape.view_origin_count; v++) {
        unsigned ordinal = 0;
        if (!xr_semantic_callable_key_literal(&cursor, ";view:") ||
            !xr_semantic_callable_key_unsigned(&cursor, XR_VIEW_ORIGIN_STATIC, &value) ||
            !xr_semantic_callable_key_literal(&cursor, ":"))
            return false;
        bool negative = xr_semantic_callable_key_literal(&cursor, "-1");
        if ((!negative && !xr_semantic_callable_key_unsigned(&cursor, INT16_MAX, &ordinal)) ||
            (value == XR_VIEW_ORIGIN_PARAM && (negative || ordinal >= shape.parameter_count)) ||
            (value != XR_VIEW_ORIGIN_PARAM && !negative) ||
            (value == XR_VIEW_ORIGIN_RECEIVER && shape.receiver_mode != XR_PARAM_READ))
            return false;
        int signed_ordinal = negative ? -1 : (int) ordinal;
        if ((int) value < previous_kind ||
            ((int) value == previous_kind && signed_ordinal <= previous_ordinal))
            return false;
        previous_kind = (int) value;
        previous_ordinal = signed_ordinal;
        if (value == XR_VIEW_ORIGIN_PARAM) {
            uint8_t mode = 0;
            while (next_parameter <= ordinal) {
                if (!xr_semantic_callable_key_parameter(&parameters, &mode, &ignored))
                    return false;
                next_parameter++;
            }
            if (mode != XR_PARAM_READ)
                return false;
        }
    }
    shape.generic_begin = cursor.at;
    for (unsigned g = 0; g < shape.generic_parameter_count; g++) {
        if (!xr_semantic_callable_key_literal(&cursor, ";generic:") ||
            !xr_semantic_callable_key_unsigned(&cursor, UINT16_MAX, &value) || value != g ||
            !xr_semantic_callable_key_literal(&cursor, ":") ||
            !xr_semantic_callable_key_component(&cursor, &ignored) || ignored.size == 0 ||
            !xr_semantic_callable_key_literal(&cursor, ":constraints:") ||
            !xr_semantic_callable_key_unsigned(&cursor, UINT16_MAX, &value))
            return false;
        for (unsigned c = 0; c < value; c++) {
            if (!xr_semantic_callable_key_literal(&cursor, ";constraint:") ||
                !xr_semantic_callable_key_component(&cursor, &ignored) || ignored.size == 0)
                return false;
        }
    }
    if (cursor.at != cursor.end)
        return false;
    *out = shape;
    return true;
}

static inline bool xr_semantic_callable_key_slice_equal(XrSemanticCallableKeySlice slice,
                                                         const char *key) {
    return key && strlen(key) == slice.size && memcmp(slice.data, key, slice.size) == 0;
}

static inline bool xr_semantic_callable_key_frozen_types(const XrSemanticPlan *plan,
                                                         const XrSemanticTypeRecord *type) {
    XrSemanticCallableKeyShape shape = {0};
    uint32_t children_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(plan, &children_count);
    if (!type || type->kind != XR_KIND_FUNCTION ||
        !xr_semantic_callable_key_parse(type->canonical_key, &shape) ||
        type->child_count != (uint32_t) shape.parameter_count + 1u ||
        type->child_begin > children_count || type->child_count > children_count - type->child_begin ||
        !children)
        return false;
    XrSemanticCallableKeyCursor cursor = {shape.parameter_begin,
                                         type->canonical_key + strlen(type->canonical_key)};
    for (unsigned p = 0; p < shape.parameter_count; p++) {
        uint8_t mode = 0;
        XrSemanticCallableKeySlice component = {0};
        const XrSemanticTypeRecord *child =
            xr_semantic_plan_type(plan, children[type->child_begin + p]);
        if (!child || !xr_semantic_callable_key_parameter(&cursor, &mode, &component) ||
            !xr_semantic_callable_key_slice_equal(component, child->canonical_key))
            return false;
    }
    const XrSemanticTypeRecord *result =
        xr_semantic_plan_type(plan, children[type->child_begin + shape.parameter_count]);
    if (!result || !xr_semantic_callable_key_slice_equal(shape.result, result->canonical_key))
        return false;
    cursor.at = shape.generic_begin;
    for (unsigned g = 0; g < shape.generic_parameter_count; g++) {
        unsigned ordinal = 0, count = 0;
        XrSemanticCallableKeySlice component = {0};
        if (!xr_semantic_callable_key_literal(&cursor, ";generic:") ||
            !xr_semantic_callable_key_unsigned(&cursor, UINT16_MAX, &ordinal) || ordinal != g ||
            !xr_semantic_callable_key_literal(&cursor, ":") ||
            !xr_semantic_callable_key_component(&cursor, &component) ||
            !xr_semantic_callable_key_literal(&cursor, ":constraints:") ||
            !xr_semantic_callable_key_unsigned(&cursor, UINT16_MAX, &count))
            return false;
        for (unsigned c = 0; c < count; c++) {
            if (!xr_semantic_callable_key_literal(&cursor, ";constraint:") ||
                !xr_semantic_callable_key_component(&cursor, &component))
                return false;
            bool found = false;
            for (uint32_t t = 0; t < xr_semantic_plan_type_count(plan); t++) {
                const XrSemanticTypeRecord *candidate = xr_semantic_plan_type(plan, t);
                if (candidate && xr_semantic_callable_key_slice_equal(component, candidate->canonical_key)) {
                    found = true;
                    break;
                }
            }
            if (!found)
                return false;
        }
    }
    return cursor.at == cursor.end;
}

#endif  // XR_SEMANTIC_CALLABLE_KEY_SHAPE_H
