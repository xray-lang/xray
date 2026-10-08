/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * array_default_source_shape.h - Public default construction metadata oracle
 *
 * KEY CONCEPT:
 *   A count parameter and a typed zero independently determine the repeated Array.
 */
#ifndef ARRAY_DEFAULT_SOURCE_SHAPE_H
#define ARRAY_DEFAULT_SOURCE_SHAPE_H
#include "xir/xxir_types.h"
#include "xir/xxir_declarations.h"
static uint32_t array_default_source_shape(const XrXirModule *module) {
    CHECK(module && module->declarations && module->types);
    uint32_t found = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *fn = &module->functions[f];
        if (fn->name_length != 12 || memcmp(fn->name, "defaultCount", 12)) continue;
        CHECK(found == UINT32_MAX && fn->parameter_count == 1 && fn->parameters[0] == XR_XIR_I64 && fn->result == XR_XIR_I64);
        const XrXirFunctionIdentity *identity = &module->declarations->functions[f];
        CHECK(identity->exported && identity->module == module->declarations->root_module);
        unsigned repeats = 0, lengths = 0;
        for (uint32_t i = 0; i < fn->instruction_count; ++i) {
            const XrXirInstruction *op = &fn->instructions[i];
            if (op->op == XR_XIR_ARRAY_REPEAT) {
                ++repeats;
                CHECK(xr_xir_array_element(module->types, op->type) == XR_XIR_U8 && xr_xir_type_is_owned(module->types, op->type));
                CHECK(op->args[0] == 0 && op->args[1] >= fn->parameter_count && op->args[1] - fn->parameter_count < i);
                const XrXirInstruction *zero = &fn->instructions[op->args[1] - fn->parameter_count];
                CHECK(zero->op == XR_XIR_CONST_INT && zero->type == XR_XIR_U8 && !zero->immediate);
                CHECK(!op->immediate && !op->targets[0] && !op->targets[1] && !op->type_arguments[0] && !op->type_arguments[1]);
            }
            if (op->op == XR_XIR_ARRAY_LEN) {
                ++lengths;
                CHECK(op->type == XR_XIR_I64 && xr_xir_array_element(module->types, xr_xir_operand_type(fn, op->args[0])) == XR_XIR_U8);
            }
        }
        CHECK(repeats == 1 && lengths == 1);
        found = f;
    }
    CHECK(found != UINT32_MAX);
    return found;
}
#endif // ARRAY_DEFAULT_SOURCE_SHAPE_H
