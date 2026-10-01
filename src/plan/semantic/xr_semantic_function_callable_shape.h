/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_function_callable_shape.h - Frozen callable declaration relation
 */


/* Frozen callable declaration facts are separate from a call-site effect
 * admission. This judgement only joins the signature to its function body. */
#ifndef XR_SEMANTIC_FUNCTION_CALLABLE_SHAPE_H
#define XR_SEMANTIC_FUNCTION_CALLABLE_SHAPE_H
#include "xr_semantic_callable_key_shape.h"
#include <stdio.h>

/* The VM collects rest arguments into one non-null Array at the body
 * boundary. The declaration describes each collected element. This join
 * preserves that ABI without admitting another container or element type. */
static inline const XrSemanticTypeRecord *xr_semantic_callable_rest_element(
    const XrSemanticPlan *plan, const XrSemanticTypeRecord *packed) {
    XrStableId zero = {{0}};
    uint32_t child_count = 0;
    const uint32_t *children = plan ? xr_semantic_plan_type_children(plan, &child_count) : NULL;
    if (!plan || !packed || packed->kind != XR_KIND_ARRAY ||
        packed->builtin_type != XR_TID_NULL || packed->child_count != 1 ||
        !children || packed->child_begin >= child_count ||
        packed->aggregate_extent != 0 || packed->aggregate_align != 0 ||
        packed->scalar_rep != XR_SCALAR_REP_NONE ||
        packed->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) ||
        packed->source_class != XR_SEMANTIC_INDEX_NONE ||
        !xr_stable_id_equal(packed->source_class_identity, zero) || !packed->canonical_key)
        return NULL;
    const XrSemanticTypeRecord *element = xr_semantic_plan_type(plan, children[packed->child_begin]);
    char prefix[96];
    int length = snprintf(prefix, sizeof(prefix),
        "type-v3:%u:0:%u:0:0:0:0:0:0:%u:0:;element:",
        (unsigned) XR_KIND_ARRAY, (unsigned) XR_TID_NULL, (unsigned) XR_SCALAR_REP_NONE);
    return element && element->canonical_key && length > 0 &&
        (size_t) length < sizeof(prefix) &&
        strncmp(packed->canonical_key, prefix, (size_t) length) == 0 &&
        strcmp(packed->canonical_key + length, element->canonical_key) == 0 ? element : NULL;
}

static inline bool xr_semantic_function_callable_shape_is_exact(
    const XrSemanticPlan *plan, const XrSemanticFunctionRecord *function) {
    if (!plan || !function || function->effect_complete > 1u ||
        (function->effect_complete &&
         (function->unknown_semantic_effects != 0 || function->effect_unknown_reasons != 0)))
        return false;
    if (function->callable_type == XR_SEMANTIC_INDEX_NONE)
        return true;
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, function->callable_type);
    XrSemanticCallableKeyShape shape = {0};
    if (!type || type->kind != XR_KIND_FUNCTION ||
        !xr_semantic_callable_key_parse(type->canonical_key, &shape) ||
        !xr_semantic_callable_key_frozen_types(plan, type))
        return false;
    /* A Source method's runtime declaration excludes its implicit receiver.
     * Receiver binding is judged by the class authority, not by this free
     * callable relation. No effect admission consumes such a missing join. */
    if (function->source_kind != XR_SEM_SOURCE_FUNCTION_NONE)
        return true;
    if (shape.parameter_count != function->parameter_count)
        return false;
    XrSemanticCallableKeyCursor cursor = {shape.parameter_begin,
        type->canonical_key + strlen(type->canonical_key)};
    unsigned required = 0, variadic = 0;
    for (uint16_t p = 0; p < function->parameter_count; p++) {
        const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(
            plan, function->parameter_begin + p);
        const XrSemanticTypeRecord *parameter_type = parameter ?
            xr_semantic_plan_type(plan, parameter->type) : NULL;
        uint8_t mode = 0;
        XrSemanticCallableKeySlice component = {0};
        if (!parameter || !parameter_type ||
            !xr_semantic_callable_key_parameter(&cursor, &mode, &component) ||
            mode != parameter->mode)
            return false;
        bool rest = (parameter->flags & XR_SEM_PARAMETER_VARIADIC) != 0;
        if (rest) {
            if (!shape.is_variadic || p + 1u != function->parameter_count ||
                (parameter->flags & XR_SEM_PARAMETER_REQUIRED) != 0)
                return false;
            parameter_type = xr_semantic_callable_rest_element(plan, parameter_type);
        }
        if (!parameter_type ||
            !xr_semantic_callable_key_slice_equal(component, parameter_type->canonical_key))
            return false;
        required += (parameter->flags & XR_SEM_PARAMETER_REQUIRED) != 0;
        variadic += (parameter->flags & XR_SEM_PARAMETER_VARIADIC) != 0;
    }
    const XrSemanticTypeRecord *result = xr_semantic_plan_type(plan, function->return_type);
    return result && shape.minimum_parameters == required &&
           shape.is_variadic == (variadic == 1u) && variadic <= 1u &&
           xr_semantic_callable_key_slice_equal(shape.result, result->canonical_key);
}
#endif
