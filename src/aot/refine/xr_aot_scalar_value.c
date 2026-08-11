/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_aot_scalar_value.c - Xi to scalar TargetPlan identity bridge
 */

#include "xr_aot_scalar_value.h"
#include <stdio.h>

static bool fail(char *error, size_t error_size, const char *detail) {
    if (error && error_size)
        snprintf(error, error_size, "XR_TARGET_1001: %s", detail);
    return false;
}

static bool block_belongs_to_function(const XiFunc *function, const XiBlock *block) {
    if (!function || !block || block->func != function)
        return false;
    for (uint32_t i = 0; i < function->nblocks; i++) {
        if (function->blocks[i] == block)
            return true;
    }
    return false;
}

static bool value_belongs_to_block(const XiBlock *block, const XiValue *value) {
    for (uint32_t i = 0; i < block->nvalues; i++) {
        if (block->values[i] == value)
            return true;
    }
    for (const XiPhi *phi = block->phis; phi; phi = phi->next) {
        if (&phi->value == value)
            return true;
    }
    return false;
}

static bool parameter_identity_is_exact(const XiFunc *function, const XiValue *value) {
    if (value->op != XI_PARAM)
        return true;
    if (!function->params || value->aux_int < 0 || value->aux_int >= function->nparams)
        return false;
    uint16_t parameter = (uint16_t) value->aux_int;
    return function->params[parameter] == value;
}

bool xr_aot_scalar_semantic_value_id(const XrTargetPlan *target_plan, const XiFunc *function,
                                     const XiValue *value, uint32_t *out_semantic_function,
                                     uint32_t *out_semantic_value, char *error,
                                     size_t error_size) {
    if (out_semantic_function)
        *out_semantic_function = XR_SEMANTIC_INDEX_NONE;
    if (out_semantic_value)
        *out_semantic_value = XR_SEMANTIC_INDEX_NONE;
    if (!target_plan || !function || !value || !out_semantic_function || !out_semantic_value)
        return fail(error, error_size, "scalar semantic identity input is missing");
    if (!xr_target_plan_is_verified(target_plan))
        return fail(error, error_size,
                    "scalar semantic identity requires a verified TargetPlan");
    const XrSemanticPlan *semantic_plan = xr_target_plan_semantic_plan(target_plan);
    if (!semantic_plan || function->semantic_plan != semantic_plan ||
        function->semantic_plan_function_index == XR_SEMANTIC_INDEX_NONE)
        return fail(error, error_size,
                    "Xi function does not carry the TargetPlan semantic authority");
    if (!block_belongs_to_function(function, value->block) ||
        !value_belongs_to_block(value->block, value) ||
        !parameter_identity_is_exact(function, value))
        return fail(error, error_size,
                    "Xi value is not an exact member of the semantic function");
    uint32_t function_index = function->semantic_plan_function_index;
    const XrSemanticFunctionRecord *semantic_function =
        xr_semantic_plan_function(semantic_plan, function_index);
    if (!semantic_function || value->id >= semantic_function->value_count ||
        semantic_function->value_begin > UINT32_MAX - value->id)
        return fail(error, error_size, "Xi scalar value identity is out of range");
    *out_semantic_function = function_index;
    *out_semantic_value = semantic_function->value_begin + value->id;
    return true;
}
